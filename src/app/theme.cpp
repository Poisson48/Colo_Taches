#include "theme.h"

#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>

namespace app {

// QStyleHints::colorScheme() n'existe qu'à partir de Qt 6.5. Android est en 6.8, mais
// le build desktop se fait avec le Qt de la distribution (6.4 sur Ubuntu 24.04) : sans
// ce garde, l'app ne compile plus du tout hors Android. Le repli lit la luminosité du
// fond de la palette système, ce qui donne la même réponse dans les cas qui comptent.
#define COLO_HAS_COLOR_SCHEME (QT_VERSION >= QT_VERSION_CHECK(6, 5, 0))

Theme::Theme(QObject *parent)
    : QObject(parent)
{
    applyColorScheme();

#if COLO_HAS_COLOR_SCHEME
    // Android bascule clair/sombre sans redémarrer l'app : suivre le changement à
    // chaud, sinon l'app reste dans l'ancien thème jusqu'au prochain lancement.
    if (auto *hints = QGuiApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged, this,
                [this](Qt::ColorScheme) { applyColorScheme(); });
    }
#endif
}

void Theme::applyColorScheme()
{
    bool dark;

    // Forçage explicite (captures d'écran, débogage) : COLO_THEME=dark|light l'emporte
    // sur la détection système.
    const QByteArray forced = qgetenv("COLO_THEME");
    if (forced == "dark") {
        dark = true;
    } else if (forced == "light") {
        dark = false;
    } else {
#if COLO_HAS_COLOR_SCHEME
        auto *hints = QGuiApplication::styleHints();
        // Système sans préférence exprimée (Qt::ColorScheme::Unknown) : on garde le
        // sombre, l'identité visuelle d'origine de l'app.
        dark = !hints || hints->colorScheme() != Qt::ColorScheme::Light;
#else
        const QColor window = QGuiApplication::palette().color(QPalette::Window);
        dark = window.lightness() < 128;
#endif
    }

    if (dark == m_dark)
        return;

    m_dark = dark;
    emit paletteChanged();
}

} // namespace app
