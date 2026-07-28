# Find libsodium and libsecp256k1 via pkg-config.
# IMPORTED_TARGET: les cibles portent aussi les -L, indispensable en cross-build
# (le préfixe Android n'est pas dans les chemins par défaut du linker).
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(SECP256K1 REQUIRED IMPORTED_TARGET libsecp256k1)

# --- qrcodegen static library (vendored from Nayuki) ---
add_library(qrcodegen STATIC
    ${CMAKE_SOURCE_DIR}/third_party/qrcodegen/qrcodegen.cpp
)
target_include_directories(qrcodegen PUBLIC ${CMAKE_SOURCE_DIR}/third_party/qrcodegen)

# --- net static library (QWebSocket, Nostr NIP-01) ---
add_library(net STATIC
    net/nostr.cpp
    net/relayclient.cpp
    net/relaypool.cpp
)

target_include_directories(net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(net PUBLIC
    Qt6::Core
    Qt6::WebSockets
)

# --- core static library (pure C++/STL, no Qt) ---
add_library(core STATIC
    core/crdt.cpp
    core/payload.cpp
    core/pairing.cpp
    core/csv.cpp
    core/zip.cpp
)

target_include_directories(core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party
)

# core is pure STL: no Qt linkage.

# --- store static library ---
add_library(store STATIC
    store/database.cpp
)

target_include_directories(store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(store PUBLIC
    Qt6::Core
    Qt6::Sql
)

# --- cryptolayer static library (net/crypto.cpp) ---
add_library(cryptolayer STATIC
    net/crypto.cpp
)

target_include_directories(cryptolayer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(cryptolayer PUBLIC
    Qt6::Core
    net
    PkgConfig::SODIUM
    PkgConfig::SECP256K1
)

# --- qrdecode : décodage QR (zxing), sans dépendance à la caméra ---
add_library(qrdecode STATIC app/qrdecode.cpp)
target_include_directories(qrdecode PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_link_libraries(qrdecode PUBLIC Qt6::Gui ZXing::ZXing)

# --- app library (AppController, ItemModel, QrImageProvider, SyncEngine) ---
set(APPLIB_SOURCES
    app/appcontroller.cpp
    app/itemmodel.cpp
    app/permissions.cpp
    app/platform.cpp
    app/qrimageprovider.cpp
    app/taskimageprovider.cpp
    app/syncengine.cpp
    app/theme.cpp
    app/updater.cpp
)
if(COLO_HAS_CAMERA)
    list(APPEND APPLIB_SOURCES app/qrscanner.cpp)
endif()

add_library(applib STATIC ${APPLIB_SOURCES})

target_include_directories(applib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(applib PUBLIC
    core
    cryptolayer
    net
    store
    qrcodegen
    qrdecode
    Qt6::Core
    Qt6::Network      # Updater : API GitHub + téléchargement de l'APK
    Qt6::Quick
    Qt6::Sql
    Qt6::WebSockets
    PkgConfig::SODIUM
    PkgConfig::SECP256K1
)

if(COLO_HAS_CAMERA)
    target_link_libraries(applib PUBLIC Qt6::Multimedia)
    target_compile_definitions(applib PUBLIC COLO_HAS_CAMERA)
endif()

if(TARGET Qt6::Widgets)
    target_link_libraries(applib PUBLIC Qt6::Widgets)
endif()

# --- main application ---
qt_add_executable(colotaches
    app/main.cpp
)

set(COLO_QML_FILES
    qml/Main.qml
    qml/ListsPage.qml
    qml/ListPage.qml
    qml/TaskPage.qml
    qml/ShareSheet.qml
    qml/ColoDialog.qml
    qml/ColoMenu.qml
    qml/ColoTextField.qml
    qml/AisleBox.qml
    qml/Icon.qml
    qml/FilePickers.qml
)
if(COLO_HAS_CAMERA)
    list(APPEND COLO_QML_FILES qml/ScanPage.qml)
endif()

qt_add_qml_module(colotaches
    URI ColoTaches
    VERSION 1.0
    QML_FILES ${COLO_QML_FILES}
)

target_link_libraries(colotaches PRIVATE
    applib
    net
    cryptolayer
    store
    qrcodegen
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
    Qt6::WebSockets
    PkgConfig::SODIUM
    PkgConfig::SECP256K1
)

# Paquet Android : manifest (permissions, label, icône), ressources, et le Java du
# pont plateforme (android/src/org/colotaches/app/Platform.java).
set_property(TARGET colotaches PROPERTY
    QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/android")
# targetSdk 34 volontairement, pas 35 : à partir d'Android 15, cibler 35 impose
# l'edge-to-edge, et Qt 6.8 ne gère pas les insets — l'UI passerait sous la barre
# de statut et sous la barre de navigation.
set_property(TARGET colotaches PROPERTY QT_ANDROID_TARGET_SDK_VERSION 34)

# Version de l'APK. scripts/build-android.sh passe ces valeurs et aligne le manifest
# dessus ; le workflow de release les dérive du tag git.
set(COLO_VERSION_NAME "0.1.0" CACHE STRING "android:versionName")
set(COLO_VERSION_CODE "1"     CACHE STRING "android:versionCode")
set_property(TARGET colotaches PROPERTY QT_ANDROID_VERSION_NAME "${COLO_VERSION_NAME}")
set_property(TARGET colotaches PROPERTY QT_ANDROID_VERSION_CODE "${COLO_VERSION_CODE}")

# La même version dans le binaire : l'Updater compare ce qu'on exécute à la dernière
# release publiée. Sans ça, il ne saurait pas de quelle version il part.
target_compile_definitions(applib PUBLIC COLO_APP_VERSION="${COLO_VERSION_NAME}")

# Android n'embarque pas OpenSSL : sans ces .so, QSslSocket est muet et les
# connexions wss:// aux relais échouent silencieusement.
if(ANDROID)
    include(FetchContent)
    FetchContent_Declare(android_openssl
        GIT_REPOSITORY https://github.com/KDAB/android_openssl.git
        GIT_TAG master
    )
    FetchContent_MakeAvailable(android_openssl)
    include(${android_openssl_SOURCE_DIR}/android_openssl.cmake)
    add_android_openssl_libraries(colotaches)
endif()
