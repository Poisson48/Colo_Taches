#pragma once

#include <QColor>
#include <QObject>

namespace app {

// Palette de l'app, exposée à QML comme propriété de contexte `Theme`.
//
// Volontairement en C++ et non en singleton QML : un `pragma Singleton` dépend de la
// résolution du qmldir du module, qui s'est révélée différente entre le Qt du desktop
// et celui d'Android — Theme y arrivait comme type ordinaire, toutes les couleurs et
// les espacements passaient à `undefined`, et les dialogues se disloquaient. Une
// propriété de contexte ne peut pas échouer ainsi.
//
// Clair ou sombre selon la préférence du système, suivie à chaud (Android bascule
// sans redémarrer l'app). Les couleurs ne sont donc plus CONSTANT.
class Theme : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool dark READ dark NOTIFY paletteChanged)

    Q_PROPERTY(QColor background  READ background  NOTIFY paletteChanged)
    Q_PROPERTY(QColor surface     READ surface     NOTIFY paletteChanged)
    Q_PROPERTY(QColor surfaceHigh READ surfaceHigh NOTIFY paletteChanged)
    Q_PROPERTY(QColor outline     READ outline     NOTIFY paletteChanged)
    Q_PROPERTY(QColor accent      READ accent      NOTIFY paletteChanged)
    // Accent assombri : état pressé d'un bouton plein, pastille de comptage.
    Q_PROPERTY(QColor accentDim   READ accentDim   NOTIFY paletteChanged)
    // Teinte douce de l'accent : fond d'une ligne sélectionnée, sur laquelle le texte
    // ordinaire doit rester lisible — ce que `accentDim` ne permet pas en thème clair.
    Q_PROPERTY(QColor accentSoft  READ accentSoft  NOTIFY paletteChanged)
    // Couleur du texte POSÉ SUR l'accent. Sans elle, les libellés des boutons pleins
    // restaient en dur (« #0C1F10 »), illisibles dès que l'accent s'assombrit.
    Q_PROPERTY(QColor onAccent    READ onAccent    NOTIFY paletteChanged)
    Q_PROPERTY(QColor text        READ text        NOTIFY paletteChanged)
    Q_PROPERTY(QColor textDim     READ textDim     NOTIFY paletteChanged)
    Q_PROPERTY(QColor danger      READ danger      NOTIFY paletteChanged)
    Q_PROPERTY(QColor warning     READ warning     NOTIFY paletteChanged)

    // Rythme vertical et cibles tactiles : 48 est le minimum tactile Android.
    Q_PROPERTY(int radius      MEMBER m_radius      CONSTANT)
    Q_PROPERTY(int touchTarget MEMBER m_touchTarget CONSTANT)
    Q_PROPERTY(int gap         MEMBER m_gap         CONSTANT)
    Q_PROPERTY(int pad         MEMBER m_pad         CONSTANT)

public:
    explicit Theme(QObject *parent = nullptr);

    bool dark() const { return m_dark; }

    QColor background()  const { return m_dark ? QColor("#101311") : QColor("#F5F7F5"); }
    QColor surface()     const { return m_dark ? QColor("#1A1F1C") : QColor("#FFFFFF"); }
    QColor surfaceHigh() const { return m_dark ? QColor("#232A25") : QColor("#EBEFEC"); }
    QColor outline()     const { return m_dark ? QColor("#2E3630") : QColor("#DCE2DD"); }
    QColor accent()      const { return m_dark ? QColor("#6FCF7A") : QColor("#2E7D32"); }
    QColor accentDim()   const { return m_dark ? QColor("#2E7D32") : QColor("#1B5E20"); }
    QColor accentSoft()  const { return m_dark ? QColor("#24402A") : QColor("#D7EDDA"); }
    QColor onAccent()    const { return m_dark ? QColor("#0C1F10") : QColor("#FFFFFF"); }
    QColor text()        const { return m_dark ? QColor("#ECF0ED") : QColor("#15201A"); }
    QColor textDim()     const { return m_dark ? QColor("#93A099") : QColor("#5F6D66"); }
    QColor danger()      const { return m_dark ? QColor("#E5534B") : QColor("#C62828"); }
    QColor warning()     const { return QColor("#E9B44C"); }

signals:
    void paletteChanged();

private:
    void applyColorScheme();

    bool m_dark = true;

    int m_radius      = 16;
    int m_touchTarget = 48;
    int m_gap         = 12;
    int m_pad         = 16;
};

} // namespace app
