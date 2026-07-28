qt_add_executable(tst_smoke
    tst_smoke.cpp
)

target_link_libraries(tst_smoke PRIVATE
    Qt6::Test
)

add_test(NAME smoke COMMAND tst_smoke)

# --- Database tests ---
qt_add_executable(tst_database
    tst_database.cpp
)

target_link_libraries(tst_database PRIVATE
    store
    Qt6::Test
    Qt6::Sql
)

add_test(NAME database COMMAND tst_database)

# --- CRDT unit tests (pure C++, no Qt) ---
add_executable(tst_crdt
    tst_crdt.cpp
)

target_include_directories(tst_crdt PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(tst_crdt PRIVATE core)

add_test(NAME crdt COMMAND tst_crdt)

# --- CSV / ZIP export-import (pure C++, no Qt) ---
add_executable(tst_dataexchange
    tst_dataexchange.cpp
)

target_include_directories(tst_dataexchange PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(tst_dataexchange PRIVATE core)

add_test(NAME dataexchange COMMAND tst_dataexchange)

# --- Payload codec tests (pure C++, no Qt) ---
add_executable(tst_payload
    tst_payload.cpp
)

target_include_directories(tst_payload PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(tst_payload PRIVATE core)

add_test(NAME payload COMMAND tst_payload)

# --- Relay / Nostr transport tests ---
qt_add_executable(tst_relay
    tst_relay.cpp
)

target_include_directories(tst_relay PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(tst_relay PRIVATE
    net
    Qt6::Test
    Qt6::WebSockets
)

add_test(NAME relay COMMAND tst_relay)

# --- ItemModel tests (QAbstractListModel + CRDT-aware slots) ---
qt_add_executable(tst_itemmodel
    tst_itemmodel.cpp
)

target_include_directories(tst_itemmodel PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(tst_itemmodel PRIVATE
    applib
    store
    Qt6::Test
    Qt6::Sql
)

add_test(NAME itemmodel COMMAND tst_itemmodel)

# --- QML : les écrans se chargent-ils, et la logique qu'ils portent tient-elle ? ---
qt_add_executable(tst_qml
    tst_qml.cpp
)

target_include_directories(tst_qml PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Les pages sont chargées depuis les sources, pas depuis le module QML compilé dans
# l'exécutable : le test voit exactement les fichiers que le développeur édite.
target_compile_definitions(tst_qml PRIVATE
    COLO_QML_DIR="${CMAKE_SOURCE_DIR}/src/qml"
)

target_link_libraries(tst_qml PRIVATE
    applib
    store
    core
    qrcodegen
    Qt6::Test
    Qt6::Quick
    Qt6::QuickControls2
)

add_test(NAME qml COMMAND tst_qml)
set_tests_properties(qml PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# --- Crypto tests (XChaCha20-Poly1305, BIP340 Schnorr, NIP-01) ---
qt_add_executable(tst_crypto
    tst_crypto.cpp
)

target_include_directories(tst_crypto PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(tst_crypto PRIVATE
    cryptolayer
    core
    net
    Qt6::Test
    Qt6::Core
)

add_test(NAME crypto COMMAND tst_crypto)

# --- Sync engine integration tests ---
qt_add_executable(tst_sync
    tst_sync.cpp
)

target_include_directories(tst_sync PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(tst_sync PRIVATE
    applib
    cryptolayer
    net
    store
    core
    Qt6::Test
    Qt6::Core
    Qt6::WebSockets
    Qt6::Sql
)

add_test(NAME sync COMMAND tst_sync)
set_tests_properties(sync PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# --- Pairing URI tests (pure C++, no Qt) ---
add_executable(tst_pairing
    tst_pairing.cpp
)

target_include_directories(tst_pairing PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(tst_pairing PRIVATE core)

add_test(NAME pairing COMMAND tst_pairing)

# --- QR decode (zxing) : le QR affiché doit être relu par l'autre appareil ---
qt_add_executable(tst_qrdecode
    tst_qrdecode.cpp
)

target_link_libraries(tst_qrdecode PRIVATE
    qrdecode
    qrcodegen
    core
    Qt6::Test
    Qt6::Gui
)

add_test(NAME qrdecode COMMAND tst_qrdecode)
# QTEST_MAIN instancie une QGuiApplication : sans display (CI, conteneur), le test
# avorte au démarrage. Le rendu du QR se fait sur une QImage, aucun écran requis.
set_tests_properties(qrdecode PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
