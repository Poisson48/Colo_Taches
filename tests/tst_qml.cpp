// tst_qml.cpp — Les écrans se chargent-ils sans erreur de binding, et la logique
// portée par le QML (sélection multiple, bouton retour) fait-elle ce qu'elle dit ?
//
// Les pages sont chargées depuis les sources (COLO_QML_DIR) avec les mêmes propriétés
// de contexte que main.cpp. Toute erreur QML — propriété absente, fonction manquante,
// binding cassé — remonte ici au lieu d'attendre le téléphone.

#include <QtTest>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QDateTime>

#include "app/appcontroller.h"
#include "app/itemmodel.h"
#include "core/types.h"
#include "store/database.h"
#include "app/qrimageprovider.h"
#include "app/theme.h"
#include "app/updater.h"

// Les avertissements QML (binding cassé, appel sur undefined) ne remontent pas dans
// QQmlComponent::errors : ils passent par le gestionnaire de messages. On les capture.
static QStringList g_warnings;

static void warningCollector(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        g_warnings << msg;
    Q_UNUSED(ctx);
}

class QmlTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine        *m_engine = nullptr;
    app::AppController m_ctrl;
    app::Theme         m_theme;
    // check() n'est pas appelé : le test ne doit pas dépendre du réseau. L'Updater
    // reste à l'état Idle, la bannière de mise à jour reste donc masquée.
    app::Updater       m_updater;

    // Tous les textes réellement posés à l'écran, en descendant l'arbre visuel. C'est
    // la seule façon de vérifier qu'une ligne affiche bien ses données, sans dépendre
    // de la structure interne des délégués.
    static QStringList visibleTexts(QQuickItem *root) {
        QStringList out;
        if (!root)
            return out;
        const QVariant text = root->property("text");
        if (text.isValid() && !text.toString().isEmpty())
            out << text.toString();
        for (QQuickItem *child : root->childItems())
            out += visibleTexts(child);
        return out;
    }

    // Instancie une page avec les propriétés de contexte de l'app réelle.
    QObject *load(const QString &file, const QVariantMap &props = {}) {
        QQmlComponent comp(m_engine,
                           QUrl::fromLocalFile(QStringLiteral(COLO_QML_DIR "/") + file));
        if (comp.status() != QQmlComponent::Ready) {
            qWarning() << "component not ready:" << comp.errorString();
            return nullptr;
        }
        return comp.createWithInitialProperties(props);
    }

private slots:
    void initTestCase() {
        // Sans le style Material, les attachements (Material.accent…) ne résolvent pas.
        QQuickStyle::setStyle(QStringLiteral("Material"));

        m_engine = new QQmlEngine(this);
        m_engine->addImageProvider(QStringLiteral("qr"), new app::QrImageProvider());
        m_engine->rootContext()->setContextProperty(QStringLiteral("AppController"), &m_ctrl);
        m_engine->rootContext()->setContextProperty(QStringLiteral("Theme"), &m_theme);
        m_engine->rootContext()->setContextProperty(QStringLiteral("Updater"), &m_updater);
    }

    // Captures d'écran pour le README. Ne tourne que si COLO_SHOT_DIR est défini
    // (sinon on n'impose pas un serveur graphique à la CI de tests). À lancer avec :
    //   QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
    //   COLO_SHOT_DIR=docs/screenshots ./tst_qml
    void screenshots() {
        const QString outDir = qEnvironmentVariable("COLO_SHOT_DIR");
        if (outDir.isEmpty())
            QSKIP("COLO_SHOT_DIR non défini");

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        store::Database &db = m_ctrl.db();
        QVERIFY(db.open(dir.filePath("shots.db")));

        // Nom défini (pas de dialogue au démarrage) et en ligne (pas de bandeau).
        m_ctrl.setDisplayName("Alex");
        QMetaObject::invokeMethod(&m_ctrl, "onSyncOnlineChanged", Q_ARG(bool, true));

        // Deux listes : une rangée dans « Maison » et partagée, une non rangée.
        core::ListMeta a; a.listId = "l-taches"; a.key = std::vector<uint8_t>(32, 2);
        a.title = "Corvées de la coloc"; a.titleVer = {1,"dev-A"}; a.lamport = 1; a.created = 100;
        db.createList(a);
        core::ListMeta b; b.listId = "l-travaux"; b.key = std::vector<uint8_t>(32, 3);
        b.title = "Travaux du garage"; b.titleVer = {1,"dev-A"}; b.lamport = 1; b.created = 200;
        db.createList(b);
        db.createGroup("g-maison", "Maison", 1000);
        db.setListGroup("l-taches", "g-maison");
        db.upsertMember("l-taches", "dev-B", "Marie", {1,"dev-B"});
        db.upsertMember("l-taches", "dev-C", "Léo",   {1,"dev-C"});

        // Tâches réparties en catégories, une déjà faite, une qui aura des étapes.
        struct Seed { const char *name, *qty, *note, *aisle; bool done; };
        const Seed seeds[] = {
            { "Passer l'aspirateur", "", "salon et couloir", "Ménage",    false },
            { "Sortir les poubelles", "", "",                "Ménage",    true  },
            { "Vider le lave-vaisselle", "", "",             "Cuisine",   false },
            { "Tondre la pelouse",   "", "",                 "Jardin",    false },
            { "Repeindre le volet",  "", "peinture dans le garage", "Bricolage", false },
        };
        // Horodatages récents pour que les dates affichées se lisent naturellement
        // (l'heure du jour, « hier »…) plutôt que « 1 Jan » (époque 1970).
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        int i = 0;
        for (const auto &s : seeds) {
            core::Item it; it.listId = "l-taches";
            it.itemId = QString("it-%1").arg(i).toStdString();
            it.created = now - (5 - i) * 60000; it.order = 1000 + i; it.by = "dev-A";
            it.name = s.name; it.qty = s.qty; it.note = s.note; it.aisle = s.aisle;
            it.done = s.done; it.doneAt = s.done ? now - 86400000 : 0;
            const core::Ver v{1, "dev-A"};
            it.nameVer = it.qtyVer = it.noteVer = it.aisleVer = it.orderVer = it.doneVer = it.delVer = v;
            db.upsertItem(it);
            ++i;
        }
        // Des étapes sous « Repeindre le volet » : la progression apparaît en liste.
        const char *stepNames[] = { "Démonter", "Poncer", "Repeindre" };
        for (int sIdx = 0; sIdx < 3; ++sIdx) {
            core::Item st; st.listId = "l-taches";
            st.itemId = QString("st-%1").arg(sIdx).toStdString();
            st.parentId = "it-4";
            st.created = now - 30000 + sIdx; st.order = 1000 + sIdx; st.by = "dev-A";
            st.name = stepNames[sIdx];
            st.done = (sIdx == 0); st.doneAt = st.done ? now - 3600000 : 0;
            const core::Ver v{1, "dev-A"};
            st.nameVer = st.qtyVer = st.noteVer = st.aisleVer = st.orderVer = st.doneVer = st.delVer = v;
            db.upsertItem(st);
        }
        // Quelques favoris fréquents pour la barre de suggestions.
        for (const char *fav : { "Poubelles", "Arrosage", "Courses", "Litière" })
            db.recordFavoriteUse(fav, "", "", now);

        qobject_cast<app::ListsModel *>(m_ctrl.lists())->reload(db, "dev-A");
        m_ctrl.items()->load(db, "l-taches", "dev-A");

        QObject *window = load(QStringLiteral("Main.qml"));
        QVERIFY(window);
        auto *win = qobject_cast<QQuickWindow *>(window);
        QVERIFY(win);
        QTest::qWait(700);
        win->grabWindow().save(outDir + "/lists.png");

        // Ouvrir la liste : l'écran des tâches, avec ses catégories.
        QMetaObject::invokeMethod(&m_ctrl, "listOpened",
                                  Q_ARG(QString, "l-taches"),
                                  Q_ARG(QString, "Corvées de la coloc"));
        QTest::qWait(700);
        win->grabWindow().save(outDir + "/list.png");

        // Mode Courses (barre de progression, lignes cochables) : on active le
        // shoppingMode du ListPage, seul objet à exposer cette propriété.
        for (QObject *o : window->findChildren<QObject *>()) {
            if (o->property("shoppingMode").isValid()) {
                o->setProperty("shoppingMode", true);
                QTest::qWait(700);
                win->grabWindow().save(outDir + "/shopping.png");
                break;
            }
        }

        delete window;
    }

    // La comparaison de versions décide si l'app propose une mise à jour : une erreur
    // ici, et soit personne n'est prévenu, soit tout le monde est harcelé.
    void test_isNewer() {
        QVERIFY(app::Updater::isNewer("v0.5.0", "0.4.0"));
        QVERIFY(app::Updater::isNewer("0.4.1",  "0.4.0"));
        QVERIFY(app::Updater::isNewer("1.0.0",  "0.9.9"));
        // Le piège d'une comparaison de chaînes : "0.10.0" < "0.9.1" lexicographiquement.
        QVERIFY(app::Updater::isNewer("0.10.0", "0.9.1"));

        QVERIFY(!app::Updater::isNewer("0.4.0", "0.4.0"));
        QVERIFY(!app::Updater::isNewer("v0.4.0", "0.4.0"));   // même version, tag préfixé
        QVERIFY(!app::Updater::isNewer("0.3.9", "0.4.0"));
        QVERIFY(!app::Updater::isNewer("0.9.1", "0.10.0"));
        QVERIFY(!app::Updater::isNewer("",      "0.4.0"));    // release sans tag

        // Composants absents = 0 : "0.4" n'est pas plus récent que "0.4.0".
        QVERIFY(!app::Updater::isNewer("0.4",   "0.4.0"));
        QVERIFY(app::Updater::isNewer("0.4.1",  "0.4"));
    }

    // Les notes affichées avant l'installation : les nouveautés, et pas les consignes
    // d'installation qui suivent (on est déjà en train d'installer).
    void test_releaseNotes() {
        const QString body =
            "## Nouveautés\n"
            "Mode Courses.\n"
            "Rayons.\n"
            "\n"
            "---\n"
            "\n"
            "## Installation (Android, arm64)\n"
            "Téléchargez l'APK ci-dessous.\n";

        const QString notes = app::Updater::notesFromBody(body);

        QVERIFY(notes.contains(QStringLiteral("Mode Courses.")));
        QVERIFY(notes.contains(QStringLiteral("Rayons.")));
        // Coupé au séparateur.
        QVERIFY(!notes.contains(QStringLiteral("Installation")));
        QVERIFY(!notes.contains(QStringLiteral("APK")));
        // Le « ## » du Markdown n'a pas de rendu ici : il ne doit pas s'afficher.
        QVERIFY(!notes.contains(QLatin1Char('#')));
        QCOMPARE(notes.left(10), QStringLiteral("Nouveautés"));

        // Une release sans séparateur reste lisible en entier.
        QCOMPARE(app::Updater::notesFromBody("Juste un correctif."),
                 QStringLiteral("Juste un correctif."));
        QVERIFY(app::Updater::notesFromBody("").isEmpty());
    }

    void init() {
        g_warnings.clear();
    }

    // La fenêtre réelle : Main.qml, sa barre, son StackView, et l'écran des listes
    // qu'il instancie — dialogue du pseudo compris, puisqu'aucun nom n'est encore
    // choisi ici. C'est le chemin de démarrage d'une installation neuve.
    void test_mainWindowLoadsCleanly() {
        qInstallMessageHandler(warningCollector);
        QObject *window = load(QStringLiteral("Main.qml"));
        qInstallMessageHandler(nullptr);

        QVERIFY(window != nullptr);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(QStringLiteral("\n"))));

        // Aucun nom choisi : l'écran d'accueil doit le demander plutôt que de laisser
        // « Moi » se diffuser aux autres participants.
        QVERIFY(!m_ctrl.hasDisplayName());

        delete window;
    }

    // Les lignes s'affichent-elles VRAIMENT ? Charger la page ne suffit pas à le dire :
    // sans articles, aucun délégué n'est instancié, et une ligne cassée passe inaperçue.
    // On ouvre donc une vraie liste, avec de vrais articles, dans la vraie fenêtre.
    void test_listPage_rendersRows() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        store::Database db;
        QVERIFY(db.open(dir.filePath("test.db")));

        core::ListMeta meta;
        meta.listId   = "list-1";
        meta.key      = std::vector<uint8_t>(32, 0x01);
        meta.title    = "Courses";
        meta.titleVer = { 1, "dev-A" };
        meta.lamport  = 1;
        meta.created  = 1000;
        QVERIFY(db.createList(meta));

        // Le modèle exposé à QML est celui de l'AppController : on le charge à la main,
        // sans init() (qui ouvrirait la base réelle et se connecterait aux relais).
        app::ItemModel *model = m_ctrl.items();
        model->load(db, "list-1", "dev-A");
        model->addItem("Lait", "1L");
        model->addItem("Papier toilette", "2", "6 couches épaisses", "Hygiène");
        QCOMPARE(model->count(), 2);

        // Peuple aussi l'écran des listes (groupes + membres), pour la capture.
        const bool listsShot = qEnvironmentVariableIsSet("COLO_SCREENSHOT_LISTS");
        if (listsShot) {
            store::Database &cdb = m_ctrl.db();
            QVERIFY(cdb.open(dir.filePath("ctrl.db")));
            core::ListMeta a; a.listId = "l-courses"; a.key = std::vector<uint8_t>(32, 2);
            a.title = "Courses maison"; a.titleVer = {1, "dev-A"}; a.lamport = 1; a.created = 100;
            cdb.createList(a);
            core::ListMeta b; b.listId = "l-boulot"; b.key = std::vector<uint8_t>(32, 3);
            b.title = "Fournitures bureau"; b.titleVer = {1, "dev-A"}; b.lamport = 1; b.created = 200;
            cdb.createList(b);
            cdb.createGroup("g-maison", "Maison", 1000);
            cdb.setListGroup("l-courses", "g-maison");
            cdb.upsertMember("l-courses", "dev-B", "Marie", {1, "dev-B"});
            cdb.upsertMember("l-courses", "dev-C", "Léo",   {1, "dev-C"});
            qobject_cast<app::ListsModel *>(m_ctrl.lists())->reload(cdb, "dev-A");
        }

        qInstallMessageHandler(warningCollector);

        QObject *window = load(QStringLiteral("Main.qml"));
        QVERIFY(window);

        // Capture de l'écran des listes (groupes + partage) avant d'ouvrir une liste.
        if (listsShot) {
            QTest::qWait(200);
            if (auto *w = qobject_cast<QQuickWindow *>(window))
                w->grabWindow().save(qEnvironmentVariable("COLO_SCREENSHOT_LISTS"));
        }

        // La page s'empile comme dans l'app : par le signal qu'émet openList().
        QMetaObject::invokeMethod(&m_ctrl, "listOpened",
                                  Q_ARG(QString, "list-1"), Q_ARG(QString, "Courses"));

        // Laisser la vue créer ses délégués (elle le fait au rendu, pas à la poussée).
        QTest::qWait(200);
        qInstallMessageHandler(nullptr);

        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(QStringLiteral("\n"))));

        // Le nom de chaque tâche doit être écrit quelque part à l'écran. S'il manque,
        // c'est que les lignes ne reçoivent plus leurs données du modèle.
        auto *win = qobject_cast<QQuickWindow *>(window);
        QVERIFY(win);
        const QStringList shown = visibleTexts(win->contentItem());
        QVERIFY2(shown.contains(QStringLiteral("Lait")),
                 qPrintable("textes affichés : " + shown.join(QStringLiteral(" | "))));
        QVERIFY(shown.contains(QStringLiteral("Papier toilette")));
        // Les détails viennent du même modèle : ils tombent avec le reste.
        QVERIFY(shown.contains(QStringLiteral("6 couches épaisses")));

        // Le menu des catégories ne se peuple qu'à l'ouverture : une liste vide ne se
        // verrait qu'au moment où l'on veut classer une tâche.
        QObject *aisleBox = window->findChild<QObject *>(QStringLiteral("addAisleBox"));
        QVERIFY2(aisleBox, "sélecteur de catégorie introuvable");

        // Le popup du ComboBox est un objet à part : c'est lui qui s'ouvre.
        auto *aislePopup = aisleBox->property("popup").value<QObject *>();
        QVERIFY(aislePopup);

        qInstallMessageHandler(warningCollector);
        g_warnings.clear();
        QMetaObject::invokeMethod(aislePopup, "open");
        QTest::qWait(150);
        qInstallMessageHandler(nullptr);

        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(QStringLiteral("\n"))));

        // Voir l'écran sans téléphone ni serveur X :
        //   QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
        //   COLO_SCREENSHOT=/tmp/ui.png ./tst_qml
        // Le rendu logiciel dessine hors écran ; c'est le seul moyen de vérifier ce qui
        // ne se voit pas (icônes manquantes, thème clair illisible) autrement qu'à l'œil.
        if (qEnvironmentVariableIsSet("COLO_SCREENSHOT"))
            win->grabWindow().save(qEnvironmentVariable("COLO_SCREENSHOT"));

        // Vérifier le modèle du sélecteur lui-même : le popup offscreen n'est pas
        // toujours peuplé au moment où on inspecte l'arbre visuel, alors que la
        // liste des catégories (défauts + utilisées) est la vraie logique à tester.
        const QStringList options = aisleBox->property("options").toStringList();
        QVERIFY2(options.contains(QStringLiteral("Cuisine")),
                 qPrintable("catégories proposées : " + options.join(QStringLiteral(" | "))));
        QVERIFY(options.contains(QStringLiteral("Sans catégorie")));

        delete window;
    }

    // Les articles ajoutés deviennent des pastilles de favoris, cliquables sous la
    // barre d'ajout. On vérifie qu'elles s'affichent réellement.
    void test_favoritesBar() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        store::Database &db = m_ctrl.db();
        if (!db.isOpen())
            QVERIFY(db.open(dir.filePath("fav.db")));

        core::ListMeta m; m.listId = "l-fav"; m.key = std::vector<uint8_t>(32, 7);
        m.title = "Courses"; m.titleVer = {1,"dev-A"}; m.lamport = 1; m.created = 100;
        db.createList(m);

        // Ajouts manuels via le modèle exposé à QML → alimentent les favoris.
        app::ItemModel *model = m_ctrl.items();
        model->load(db, "l-fav", "dev-A");
        model->addItem("Bananes", "", "", "");
        model->addItem("Café", "250 g", "", "Épicerie salée");
        QVERIFY(m_ctrl.favorites().size() >= 2);

        QObject *window = load(QStringLiteral("Main.qml"));
        QVERIFY(window);
        QMetaObject::invokeMethod(&m_ctrl, "listOpened",
                                  Q_ARG(QString, "l-fav"), Q_ARG(QString, "Courses"));
        QTest::qWait(200);

        auto *win = qobject_cast<QQuickWindow *>(window);
        QVERIFY(win);
        const QStringList shown = visibleTexts(win->contentItem());
        // Les noms des favoris apparaissent en pastilles.
        QVERIFY2(shown.contains(QStringLiteral("Bananes")),
                 qPrintable("textes : " + shown.join(QStringLiteral(" | "))));
        QVERIFY(shown.contains(QStringLiteral("Café")));

        delete window;
    }

    // La gestion des rayons (écran des listes) affiche les rayons personnalisés.
    void test_aisleManagementDialog() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        store::Database &db = m_ctrl.db();
        if (!db.isOpen())
            QVERIFY(db.open(dir.filePath("am.db")));

        core::ListMeta m; m.listId = "l-am"; m.key = std::vector<uint8_t>(32, 9);
        m.title = "Courses"; m.titleVer = {1,"dev-A"}; m.lamport = 1; m.created = 100;
        db.createList(m);
        core::Item it; it.listId = "l-am"; it.itemId = "i-am"; it.created = 100;
        it.name = "Vin"; it.aisle = "Cellier"; it.nameVer = {1,"dev-A"}; it.aisleVer = {1,"dev-A"};
        db.upsertItem(it);
        QVERIFY(m_ctrl.customAisles().contains(QStringLiteral("Cellier")));

        QObject *window = load(QStringLiteral("Main.qml"));
        QVERIFY(window);
        QTest::qWait(100);

        QObject *dlg = window->findChild<QObject *>(QStringLiteral("aislesDialog"));
        QVERIFY2(dlg, "dialogue de gestion des rayons introuvable");
        QMetaObject::invokeMethod(dlg, "open");
        QTest::qWait(150);

        auto *win = qobject_cast<QQuickWindow *>(window);
        const QStringList shown = visibleTexts(win->contentItem());
        QVERIFY2(shown.contains(QStringLiteral("Cellier")),
                 qPrintable("textes : " + shown.join(QStringLiteral(" | "))));

        if (qEnvironmentVariableIsSet("COLO_SHOT_DIR"))
            win->grabWindow().save(qEnvironmentVariable("COLO_SHOT_DIR") + "/aisles.png");

        delete window;
    }

    // ListPage se charge seule (elle n'ouvre aucun popup au chargement).
    void test_listPageLoadsCleanly() {
        qInstallMessageHandler(warningCollector);
        QObject *page = load(QStringLiteral("ListPage.qml"),
                             { {"listId", "list-1"}, {"listTitle", "Courses"} });
        qInstallMessageHandler(nullptr);

        QVERIFY(page != nullptr);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(QStringLiteral("\n"))));
        delete page;
    }

    // La page d'une tâche affiche son nom, ses détails et ses étapes.
    void test_taskPageRenders() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        store::Database &db = m_ctrl.db();
        QVERIFY(db.open(dir.filePath("task.db")));
        core::ListMeta m; m.listId = "l-task"; m.key = std::vector<uint8_t>(32, 4);
        m.title = "Travaux"; m.titleVer = {1, "dev-A"}; m.lamport = 1; m.created = 100;
        QVERIFY(db.createList(m));

        m_ctrl.items()->load(db, "l-task", "dev-A");
        m_ctrl.items()->addItem("Repeindre le volet", "", "sous la fenêtre", "Bricolage");
        const QString taskId = m_ctrl.items()->data(m_ctrl.items()->index(0),
                                                    app::ItemModel::ItemIdRole).toString();

        // Deux étapes sous la tâche, chargées dans le modèle exposé à QML.
        m_ctrl.steps()->load(db, "l-task", "dev-A", taskId.toStdString());
        m_ctrl.steps()->addItem("Poncer", "");
        m_ctrl.steps()->addItem("Repeindre", "");

        g_warnings.clear();
        qInstallMessageHandler(warningCollector);
        QObject *page = load(QStringLiteral("TaskPage.qml"),
                             { {"listId", "l-task"}, {"itemId", taskId} });
        qInstallMessageHandler(nullptr);
        QVERIFY(page != nullptr);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(QStringLiteral("\n"))));

        // Le titre de la page est le nom de la tâche.
        QCOMPARE(page->property("pageTitle").toString(),
                 QStringLiteral("Repeindre le volet"));

        delete page;
    }

    // Le classement est un interrupteur unique « Classement manuel » : coché = manuel,
    // décoché = par rayon. Impossible de se retrouver sans mode (c'était le bug des deux
    // cases). Son état reflète le modèle (répliqué), y compris sur changement distant.
    void test_sortModeMenuToggle() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        store::Database &db = m_ctrl.db();
        QVERIFY(db.open(dir.filePath("sortmenu.db")));
        core::ListMeta m; m.listId = "list-1"; m.key = std::vector<uint8_t>(32, 1);
        m.title = "Courses"; m.titleVer = {1, "dev-A"}; m.lamport = 1; m.created = 100;
        QVERIFY(db.createList(m));
        m_ctrl.items()->load(db, "list-1", "dev-A");   // manualSort = false au départ

        QObject *page = load(QStringLiteral("ListPage.qml"),
                             { {"listId", "list-1"}, {"listTitle", "Courses"} });
        QVERIFY(page);
        QObject *menu = page->findChild<QObject *>(QStringLiteral("pageMenu"));
        QVERIFY(menu);

        QQuickItem *toggle = nullptr;
        const int cnt = menu->property("count").toInt();
        for (int i = 0; i < cnt; ++i) {
            QQuickItem *it = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, it), Q_ARG(int, i));
            if (it && it->property("text").toString() == QStringLiteral("Classement manuel"))
                toggle = it;
        }
        QVERIFY(toggle);
        QVERIFY(toggle->property("checkable").toBool());

        // Décoché quand la liste est par rayon.
        QVERIFY(!toggle->property("checked").toBool());
        // Passe en manuel (via le modèle) → l'interrupteur se coche.
        m_ctrl.items()->setManualSort(true);
        QVERIFY(toggle->property("checked").toBool());
        // Retour par rayon → décoché.
        m_ctrl.items()->setManualSort(false);
        QVERIFY(!toggle->property("checked").toBool());
        delete page;
    }

    // L'écran des listes se charge sans erreur de binding. Ne rend pas les délégués
    // (offscreen), mais valide le niveau supérieur : barre, menu, bindings.
    void test_listsPageLoadsCleanly() {
        g_warnings.clear();
        qInstallMessageHandler(warningCollector);
        QObject *page = load(QStringLiteral("ListsPage.qml"));
        qInstallMessageHandler(nullptr);

        // Chargée hors fenêtre, la page ouvre son dialogue de nom (onCompleted) sans
        // fenêtre où l'ancrer : artefact du harnais, sans rapport avec les bindings.
        g_warnings.removeIf([](const QString &w){ return w.contains(QStringLiteral("open popup")); });

        QVERIFY(page != nullptr);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(QStringLiteral("\n"))));
        delete page;
    }

    // Sélection multiple : la page compte ce qui est sélectionné et le dit dans le titre.
    void test_listPage_selection() {
        QObject *page = load(QStringLiteral("ListPage.qml"),
                             { {"listId", "list-1"}, {"listTitle", "Courses"} });
        QVERIFY(page);

        QCOMPARE(page->property("selectionMode").toBool(), false);
        QCOMPARE(page->property("pageTitle").toString(), QStringLiteral("Courses"));

        QMetaObject::invokeMethod(page, "toggleSelection", Q_ARG(QVariant, "item-a"));
        QCOMPARE(page->property("selectionMode").toBool(), true);
        QCOMPARE(page->property("pageTitle").toString(), QStringLiteral("1 sélectionné"));

        QMetaObject::invokeMethod(page, "toggleSelection", Q_ARG(QVariant, "item-b"));
        QCOMPARE(page->property("pageTitle").toString(), QStringLiteral("2 sélectionnés"));

        // Re-sélectionner le même article le désélectionne.
        QMetaObject::invokeMethod(page, "toggleSelection", Q_ARG(QVariant, "item-a"));
        QCOMPARE(page->property("pageTitle").toString(), QStringLiteral("1 sélectionné"));

        delete page;
    }

    // Le retour (bouton Android, flèche) sort d'abord de la sélection, et ne quitte
    // la page que s'il n'y a plus rien à annuler : c'est ce contrat que Main.qml lit.
    void test_listPage_handleBack() {
        QObject *page = load(QStringLiteral("ListPage.qml"),
                             { {"listId", "list-1"}, {"listTitle", "Courses"} });
        QVERIFY(page);

        QVariant handled;

        // Rien de sélectionné : le retour n'est pas absorbé → la page se dépile.
        QMetaObject::invokeMethod(page, "handleBack", Q_RETURN_ARG(QVariant, handled));
        QCOMPARE(handled.toBool(), false);

        // En sélection : le retour est absorbé et vide la sélection.
        QMetaObject::invokeMethod(page, "toggleSelection", Q_ARG(QVariant, "item-a"));
        QMetaObject::invokeMethod(page, "handleBack", Q_RETURN_ARG(QVariant, handled));
        QCOMPARE(handled.toBool(), true);
        QCOMPARE(page->property("selectionMode").toBool(), false);

        // La sélection vidée, le retour redevient un « quitter la page ».
        QMetaObject::invokeMethod(page, "handleBack", Q_RETURN_ARG(QVariant, handled));
        QCOMPARE(handled.toBool(), false);

        // Recherche ouverte : le retour la referme au lieu de quitter la liste.
        page->setProperty("searchOpen", true);
        QMetaObject::invokeMethod(page, "handleBack", Q_RETURN_ARG(QVariant, handled));
        QCOMPARE(handled.toBool(), true);
        QCOMPARE(page->property("searchOpen").toBool(), false);

        // Mode Courses : le retour en sort au lieu de quitter la liste.
        page->setProperty("shoppingMode", true);
        QMetaObject::invokeMethod(page, "handleBack", Q_RETURN_ARG(QVariant, handled));
        QCOMPARE(handled.toBool(), true);
        QCOMPARE(page->property("shoppingMode").toBool(), false);

        // La sélection passe avant le mode Courses : on défait le plus récent d'abord.
        page->setProperty("shoppingMode", true);
        QMetaObject::invokeMethod(page, "toggleSelection", Q_ARG(QVariant, "item-a"));
        QMetaObject::invokeMethod(page, "handleBack", Q_RETURN_ARG(QVariant, handled));
        QCOMPARE(handled.toBool(), true);
        QCOMPARE(page->property("selectionMode").toBool(), false);
        QCOMPARE(page->property("shoppingMode").toBool(), true);  // toujours en courses

        delete page;
    }

    // Réordonnancement des listes par les flèches ↑/↓ : elles appellent moveList sur
    // des index adjacents. On vérifie ici cette mécanique (un tap = un cran), de façon
    // déterministe, au niveau du modèle exposé à l'UI.
    void test_moveList_adjacent() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        store::Database &db = m_ctrl.db();
        QVERIFY(db.open(dir.filePath("movelistadj.db")));

        auto makeList = [&](const std::string &id, const std::string &title, int64_t created) {
            core::ListMeta mm; mm.listId = id; mm.key = std::vector<uint8_t>(32, 1);
            mm.title = title; mm.titleVer = {1, "dev-A"}; mm.lamport = 1; mm.created = created;
            QVERIFY(db.createList(mm));
        };
        makeList("l-a", "A", 100);
        makeList("l-b", "B", 200);
        makeList("l-c", "C", 300);
        auto *lists = qobject_cast<app::ListsModel *>(m_ctrl.lists());
        QVERIFY(lists);
        lists->reload(db, "dev-A");
        const auto nameAt = [&](int r){
            return lists->data(lists->index(r), app::ListsModel::NameRole).toString();
        };

        // Flèche ↓ sur A (index 0) → A passe en 2e position : B, A, C.
        m_ctrl.moveList(0, 1);
        QCOMPARE(nameAt(0), QStringLiteral("B"));
        QCOMPARE(nameAt(1), QStringLiteral("A"));
        // Flèche ↑ sur A (index 1) → il remonte : A, B, C.
        m_ctrl.moveList(1, 0);
        QCOMPARE(nameAt(0), QStringLiteral("A"));
        QCOMPARE(nameAt(1), QStringLiteral("B"));
    }

    // Importer le contenu d'une liste dans une autre : la destination reçoit les
    // articles « à acheter », la source reste intacte (liste-modèle réutilisable).
    void test_importListInto() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        store::Database &db = m_ctrl.db();
        QVERIFY(db.open(dir.filePath("import.db")));

        auto makeList = [&](const std::string &id, const std::string &title) {
            core::ListMeta m; m.listId = id; m.key = std::vector<uint8_t>(32, 1);
            m.title = title; m.titleVer = {1, "dev-A"}; m.lamport = 1; m.created = 1000;
            QVERIFY(db.createList(m));
        };
        auto addItem = [&](const std::string &list, const std::string &id,
                           const std::string &name, bool done) {
            core::Item it; it.listId = list; it.itemId = id; it.created = 2000;
            it.by = "dev-A"; it.name = name; it.done = done;
            const core::Ver v{1, "dev-A"};
            it.nameVer = it.qtyVer = it.noteVer = it.aisleVer = it.orderVer =
                it.doneVer = it.delVer = v;
            QVERIFY(db.upsertItem(it));
        };

        // Destination « Semaine » avec un article déjà présent (« Eau », déjà pris).
        makeList("l-dest", "Semaine");
        addItem("l-dest", "d-eau", "Eau", true);
        // Source « Courants » : deux à acheter, plus un supprimé (ne doit pas être repris).
        makeList("l-src", "Courants");
        addItem("l-src", "s-bieres", "Bières", false);
        addItem("l-src", "s-pq",     "PQ",     false);
        addItem("l-src", "s-old",    "Ancien", false);
        {
            core::Item t; t.listId = "l-src"; t.itemId = "s-old"; t.del = true;
            t.delVer = {2, "dev-A"};
            QVERIFY(db.upsertItem(t));
        }

        m_ctrl.importListInto("l-dest", "l-src");

        // La destination a ses 3 articles vivants (Eau + Bières + PQ), l'ancien exclu.
        QStringList destNames;
        bool eauStillDone = false, importedAllToBuy = true;
        for (const auto &it : db.getItems("l-dest")) {
            if (it.del) continue;
            destNames << QString::fromStdString(it.name);
            if (it.name == "Eau") eauStillDone = it.done;   // l'existant n'est pas touché
            else if (it.done) importedAllToBuy = false;      // les importés sont à acheter
        }
        destNames.sort();
        QCOMPARE(destNames, (QStringList{"Bières", "Eau", "PQ"}));
        QVERIFY(eauStillDone);          // « Eau » garde son état coché
        QVERIFY(importedAllToBuy);      // « Bières » et « PQ » arrivent à acheter

        // La source reste intacte : ses deux articles vivants sont toujours là.
        int srcAlive = 0;
        for (const auto &it : db.getItems("l-src"))
            if (!it.del) ++srcAlive;
        QCOMPARE(srcAlive, 2);

        // S'importer soi-même est sans effet (pas de doublon).
        const int before = static_cast<int>(db.getItems("l-dest").size());
        m_ctrl.importListInto("l-dest", "l-dest");
        QCOMPARE(static_cast<int>(db.getItems("l-dest").size()), before);
    }

    // Réordonnancement des listes : moveList change l'ordre du modèle, le persiste, et
    // franchir un groupe y range la liste. (Le geste tactile reste à valider au doigt ;
    // ici on vérifie la logique, testable de façon déterministe.)
    void test_moveList() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        store::Database &db = m_ctrl.db();
        QVERIFY(db.open(dir.filePath("movelist.db")));

        auto makeList = [&](const std::string &id, const std::string &title, int64_t created) {
            core::ListMeta m; m.listId = id; m.key = std::vector<uint8_t>(32, 1);
            m.title = title; m.titleVer = {1, "dev-A"}; m.lamport = 1; m.created = created;
            QVERIFY(db.createList(m));
        };
        makeList("l-a", "A", 100);
        makeList("l-b", "B", 200);
        makeList("l-c", "C", 300);

        auto *lists = qobject_cast<app::ListsModel *>(m_ctrl.lists());
        QVERIFY(lists);
        lists->reload(db, "dev-A");

        const auto nameAt = [&](int r){
            return lists->data(lists->index(r), app::ListsModel::NameRole).toString();
        };
        QCOMPARE(nameAt(0), QStringLiteral("A"));
        QCOMPARE(nameAt(2), QStringLiteral("C"));

        // Descendre A tout en bas → B, C, A.
        m_ctrl.moveList(0, 2);
        QCOMPARE(nameAt(0), QStringLiteral("B"));
        QCOMPARE(nameAt(1), QStringLiteral("C"));
        QCOMPARE(nameAt(2), QStringLiteral("A"));

        // L'ordre est persisté : un rechargement depuis la base le retrouve.
        lists->reload(db, "dev-A");
        QCOMPARE(nameAt(0), QStringLiteral("B"));
        QCOMPARE(nameAt(2), QStringLiteral("A"));

        // Remonter A en tête → A, B, C.
        m_ctrl.moveList(2, 0);
        QCOMPARE(nameAt(0), QStringLiteral("A"));
        QCOMPARE(nameAt(1), QStringLiteral("B"));
        QCOMPARE(nameAt(2), QStringLiteral("C"));

        // Franchir un groupe : ranger C dans « Maison », puis y glisser A → A rejoint
        // le groupe. Maison (ordre 1000) passe avant les listes non rangées.
        QVERIFY(db.createGroup("g-maison", "Maison", 1000));
        QVERIFY(db.setListGroup("l-c", "g-maison"));
        lists->reload(db, "dev-A");
        // Sections : Maison d'abord (C), puis non rangées (A, B).
        QCOMPARE(nameAt(0), QStringLiteral("C"));
        // Glisser A (index 1) sur la ligne de C (index 0) → A rejoint Maison.
        m_ctrl.moveList(1, 0);
        QCOMPARE(db.getList("l-a")->groupId, std::string("g-maison"));
    }
};

QTEST_MAIN(QmlTest)
#include "tst_qml.moc"
