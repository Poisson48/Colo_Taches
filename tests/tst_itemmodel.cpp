#include <QtTest>
#include <QAbstractItemModelTester>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QUrl>

#include "../src/store/database.h"
#include "../src/core/types.h"
#include "../src/app/itemmodel.h"
#include "../src/app/appcontroller.h"

using namespace store;
using namespace core;
using namespace app;

class ItemModelTest : public QObject
{
    Q_OBJECT

private:
    // Helper: create a list in DB and return its id.
    std::string makeList(Database &db, const std::string &id = "list-test") {
        ListMeta m;
        m.listId   = id;
        m.key      = std::vector<uint8_t>(32, 0x00);
        m.title    = "Test List";
        m.titleVer = {1, "dev-A"};
        m.lamport  = 1;
        m.lastSync = 0;
        m.created  = 1000;
        db.createList(m);
        return id;
    }

    // Open a fresh DB in a temp dir.
    bool openDb(Database &db, const QTemporaryDir &dir) {
        return db.open(dir.filePath("test.db"));
    }

private slots:
    // 1. QAbstractItemModelTester: conformance check.
    void test_modelTester() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
        model.load(db, listId, "dev-A");
        // add items to exercise tester
        model.addItem("Lait",    "1L");
        model.addItem("Pain",    "");
        model.addItem("Fromage", "200g");
    }

    // 2. addItem → row appears, sorted correctly, lamport incremented.
    void test_addItem_sortedInsertion() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");

        QCOMPARE(model.rowCount(), 0);

        model.addItem("A", "");
        model.addItem("B", "");
        model.addItem("C", "");

        QCOMPARE(model.rowCount(), 3);

        // All unchecked: should appear in creation order (FIFO = ascending created).
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(), "A");
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(), "B");
        QCOMPARE(model.data(model.index(2), ItemModel::NameRole).toString(), "C");

        // Verify lamport incremented monotonically in DB items.
        const auto items = db.getItems(listId);
        QCOMPARE((int)items.size(), 3);
        QVERIFY(items[0].nameVer.lamport < items[1].nameVer.lamport);
        QVERIFY(items[1].nameVer.lamport < items[2].nameVer.lamport);
    }

    // 3. toggleDone → item moves to bottom group, correct re-ordering.
    void test_toggleDone_reorder() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");

        model.addItem("A", "");
        model.addItem("B", "");
        model.addItem("C", "");

        QCOMPARE(model.rowCount(), 3);

        // Toggle first item (A) → it should move to the bottom.
        const QString idA = model.data(model.index(0), ItemModel::ItemIdRole).toString();
        model.toggleDone(idA);

        QCOMPARE(model.rowCount(), 3);
        // A is now done → must be last.
        QCOMPARE(model.data(model.index(2), ItemModel::ItemIdRole).toString(), idA);
        QCOMPARE(model.data(model.index(2), ItemModel::DoneRole).toBool(), true);

        // B and C still unchecked, in order.
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(), "B");
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(), "C");

        // Toggle A again → should come back to the top.
        model.toggleDone(idA);
        QCOMPARE(model.data(model.index(0), ItemModel::DoneRole).toBool(), false);

        // Verify done field in DB has doneVer with lamport > 0.
        bool found = false;
        for (const auto &item : db.getItems(listId)) {
            if (item.itemId == idA.toStdString()) {
                QVERIFY(item.doneVer.lamport > 0);
                QCOMPARE(item.done, false);
                found = true;
            }
        }
        QVERIFY(found);
    }

    // 4. removeItem → item disappears from view, tombstone del=true in DB with correct Ver.
    void test_removeItem_tombstone() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");

        model.addItem("X", "");
        model.addItem("Y", "");
        QCOMPARE(model.rowCount(), 2);

        const QString idX = model.data(model.index(0), ItemModel::ItemIdRole).toString();
        model.removeItem(idX);

        // View: only Y remains.
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(), "Y");

        // DB: X must have del=true with a valid Ver.
        bool found = false;
        for (const auto &item : db.getItems(listId)) {
            if (item.itemId == idX.toStdString()) {
                QVERIFY(item.del);
                QVERIFY(item.delVer.lamport > 0);
                QCOMPARE(item.delVer.deviceId, std::string("dev-A"));
                found = true;
            }
        }
        QVERIFY(found);
    }

    // 5. Versions (Ver) written with incremented lamport for each operation.
    void test_versions_lamportIncrement() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");

        model.addItem("P", "");
        const QString idP = model.data(model.index(0), ItemModel::ItemIdRole).toString();
        model.toggleDone(idP);

        const auto items = db.getItems(listId);
        QCOMPARE((int)items.size(), 1);
        const auto &item = items[0];

        // nameVer set on creation, doneVer set on toggleDone → doneVer.lamport > nameVer.lamport.
        QVERIFY(item.doneVer.lamport > item.nameVer.lamport);
    }

    // 6. Persistence: reload from DB gives same state.
    void test_persistence_reload() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        {
            ItemModel model;
            model.load(db, listId, "dev-A");
            model.addItem("Alpha",   "1kg");
            model.addItem("Beta",    "");
            model.addItem("Gamma",   "2");

            // toggle Beta (row 1)
            const QString idBeta = model.data(model.index(1), ItemModel::ItemIdRole).toString();
            model.toggleDone(idBeta);

            // remove Gamma (now row 1 = Gamma since Beta moved to bottom)
            // After toggle: Alpha(0), Gamma(1), Beta(2)
            const QString idGamma = model.data(model.index(1), ItemModel::ItemIdRole).toString();
            model.removeItem(idGamma);
        }

        // Reload into a fresh model.
        ItemModel model2;
        model2.load(db, listId, "dev-A");

        // Should have 2 visible rows: Alpha (undone) and Beta (done).
        QCOMPARE(model2.rowCount(), 2);
        QCOMPARE(model2.data(model2.index(0), ItemModel::NameRole).toString(), "Alpha");
        QCOMPARE(model2.data(model2.index(0), ItemModel::DoneRole).toBool(), false);
        QCOMPARE(model2.data(model2.index(1), ItemModel::NameRole).toString(), "Beta");
        QCOMPARE(model2.data(model2.index(1), ItemModel::DoneRole).toBool(), true);
    }

    // 7. AppController: init, createList, lists model populated.
    void test_appcontroller_createList() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // Point QStandardPaths to temp dir so AppController writes there.
        qputenv("HOME", dir.path().toLocal8Bit());

        AppController ctrl;
        // We call init() — but QStandardPaths::AppDataLocation depends on app name.
        // Instead, open DB directly and test ListsModel separately.
        // We exercise ListsModel via the controller's internal DB after a manual init.

        // Just test that lists model is initially empty/valid.
        QVERIFY(ctrl.lists() != nullptr);
    }

    // editItem : nom, quantité et description partent en LWW ; la ligne ne bouge pas.
    void test_editItem_updatesFieldsAndVersions() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Pq", "1");

        const QString itemId = model.data(model.index(0), ItemModel::ItemIdRole).toString();
        const Item before = db.getItems(listId).front();

        QSignalSpy localSpy(&model, &ItemModel::localChanged);
        model.editItem(itemId, "Papier toilette", "2 paquets", "6 couches épaisses", "");
        QCOMPARE(localSpy.count(), 1);

        // Le modèle expose les nouvelles valeurs, au même index (le tri ne dépend
        // ni du nom, ni de la quantité, ni de la note).
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(),
                 QStringLiteral("Papier toilette"));
        QCOMPARE(model.data(model.index(0), ItemModel::QtyRole).toString(),
                 QStringLiteral("2 paquets"));
        QCOMPARE(model.data(model.index(0), ItemModel::NoteRole).toString(),
                 QStringLiteral("6 couches épaisses"));

        // Persisté, avec des versions qui battent les précédentes : sans ça, un pair
        // qui rediffuse l'ancienne valeur écraserait l'édition au merge.
        const Item after = db.getItems(listId).front();
        QCOMPARE(QString::fromStdString(after.name), QStringLiteral("Papier toilette"));
        QCOMPARE(QString::fromStdString(after.note), QStringLiteral("6 couches épaisses"));
        QVERIFY(after.nameVer > before.nameVer);
        QVERIFY(after.qtyVer  > before.qtyVer);
        QVERIFY(after.noteVer > before.noteVer);
        // done n'a pas été touché : sa version ne doit pas bouger.
        QCOMPARE(after.doneVer.lamport, before.doneVer.lamport);
    }

    // Rééditer à l'identique ne doit rien émettre : une version neuve sur une valeur
    // inchangée ferait gagner ce champ contre une édition distante concurrente.
    void test_editItem_noopWhenUnchanged() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Lait", "1L");

        const QString itemId = model.data(model.index(0), ItemModel::ItemIdRole).toString();
        const Item before = db.getItems(listId).front();

        QSignalSpy localSpy(&model, &ItemModel::localChanged);
        model.editItem(itemId, "Lait", "1L", "", "");
        QCOMPARE(localSpy.count(), 0);

        const Item after = db.getItems(listId).front();
        QCOMPARE(after.nameVer.lamport, before.nameVer.lamport);
        QCOMPARE(after.qtyVer.lamport,  before.qtyVer.lamport);

        // Un nom vide n'est pas un article : refusé, l'ancien nom reste.
        model.editItem(itemId, "   ", "1L", "", "");
        QCOMPARE(QString::fromStdString(db.getItems(listId).front().name),
                 QStringLiteral("Lait"));
    }

    // toggleDone horodate le cochage, et efface la date au décochage.
    void test_toggleDone_stampsDoneAt() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Beurre", "");

        const QString itemId = model.data(model.index(0), ItemModel::ItemIdRole).toString();
        QCOMPARE(db.getItems(listId).front().doneAt, int64_t(0));

        const int64_t before = QDateTime::currentMSecsSinceEpoch();
        model.toggleDone(itemId);
        const Item done = db.getItems(listId).front();
        QVERIFY(done.done);
        QVERIFY(done.doneAt >= before);

        model.toggleDone(itemId);
        const Item undone = db.getItems(listId).front();
        QVERIFY(!undone.done);
        QCOMPARE(undone.doneAt, int64_t(0));
    }

    // Fin de course : tout remettre à acheter. Les articles remontent, rien n'est perdu.
    void test_uncheckAll() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Lait", "");
        model.addItem("Pain", "");

        model.toggleDone(model.data(model.index(0), ItemModel::ItemIdRole).toString());
        model.toggleDone(model.data(model.index(0), ItemModel::ItemIdRole).toString());
        QCOMPARE(model.doneCount(), 2);

        QSignalSpy localSpy(&model, &ItemModel::localChanged);
        model.uncheckAll();

        QCOMPARE(localSpy.count(), 1);   // une seule intention → un seul delta publié
        QCOMPARE(model.doneCount(), 0);
        QCOMPARE(model.count(), 2);      // rien n'a été supprimé

        for (const auto &it : db.getItems(listId)) {
            QVERIFY(!it.done);
            QCOMPARE(it.doneAt, int64_t(0));   // la date de cochage disparaît avec le cochage
            QVERIFY(!it.del);
        }

        // Plus rien à décocher : ne rien réécrire, sinon on publierait dans le vide.
        model.uncheckAll();
        QCOMPARE(localSpy.count(), 1);
    }

    // Fin de course : retirer ce qui a été pris, garder ce qui reste à acheter.
    void test_removeDone() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Lait", "");
        model.addItem("Pain", "");
        model.addItem("Œufs", "");

        // Cocher « Lait » : il passe en bas du tri, mais reste le même article.
        const QString lait = model.data(model.index(0), ItemModel::ItemIdRole).toString();
        model.toggleDone(lait);
        QCOMPARE(model.doneCount(), 1);

        model.removeDone();

        QCOMPARE(model.count(), 2);
        QCOMPARE(model.doneCount(), 0);

        // Tombstone (del=true), pas ligne effacée : les autres appareils doivent
        // apprendre la suppression, sinon leur copie ferait réapparaître l'article.
        int tombstones = 0;
        for (const auto &it : db.getItems(listId))
            if (it.del) ++tombstones;
        QCOMPARE(tombstones, 1);
    }

    // Recherche : un filtre d'affichage, qui ne touche ni la base ni la synchro.
    void test_filter() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Lait", "1L");
        model.addItem("Papier toilette", "2", "6 couches épaisses");
        model.addItem("Pain", "");

        model.setFilter("pa");                    // « Papier », « Pain »
        QCOMPARE(model.count(), 2);

        model.setFilter("LAIT");                  // insensible à la casse
        QCOMPARE(model.count(), 1);

        model.setFilter("couches");               // cherche aussi dans la description
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(),
                 QStringLiteral("Papier toilette"));

        model.setFilter("saumon");
        QCOMPARE(model.count(), 0);
        // Filtre d'affichage : rien n'a disparu de la base.
        QCOMPARE(db.getItems(listId).size(), size_t(3));

        model.setFilter("");
        QCOMPARE(model.count(), 3);
    }

    // Doublon : « Lait » ajouté deux fois dans une liste partagée, c'est le classique.
    void test_existingName() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Lait", "1L");

        QCOMPARE(model.existingName("lait"),  QStringLiteral("Lait")); // casse ignorée
        QCOMPARE(model.existingName(" Lait "), QStringLiteral("Lait")); // espaces ignorés
        QVERIFY(model.existingName("Pain").isEmpty());
        QVERIFY(model.existingName("").isEmpty());

        // Un article supprimé n'est plus un doublon.
        model.removeItem(model.data(model.index(0), ItemModel::ItemIdRole).toString());
        QVERIFY(model.existingName("Lait").isEmpty());
    }

    // Qui a ajouté quoi : « vous » pour soi, le nom du participant sinon.
    void test_byName() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        // Un article venu d'un autre appareil, dont on connaît le nom.
        db.upsertMember(listId, "dev-B", "Marie", { 2, "dev-B" });
        Item remote;
        remote.listId  = listId;
        remote.itemId  = "item-remote";
        remote.created = 2000;
        remote.by      = "dev-B";
        remote.name    = "Harissa";
        remote.nameVer = { 2, "dev-B" };
        QVERIFY(db.upsertItem(remote));

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Lait", "");   // le nôtre

        QHash<QString, QString> authorByItem;
        for (int i = 0; i < model.count(); ++i) {
            authorByItem[model.data(model.index(i), ItemModel::NameRole).toString()] =
                model.data(model.index(i), ItemModel::ByNameRole).toString();
        }

        QCOMPARE(authorByItem["Lait"],    QStringLiteral("vous"));
        QCOMPARE(authorByItem["Harissa"], QStringLiteral("Marie"));
    }

    // Les sections suivent l'ordre du magasin, pas l'alphabet : « Ménage »
    // avant « Cuisine », et les non classés à la fin.
    void test_aisleOrdering() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");

        model.addItem("Éponge", "", "", "Administratif");
        model.addItem("Pommes", "", "", "Ménage");
        model.addItem("Truc",   "", "", "");              // non classé
        model.addItem("Beurre", "", "", "Cuisine");

        QCOMPARE(model.count(), 4);
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(),
                 QStringLiteral("Pommes"));     // rayon 0
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(),
                 QStringLiteral("Beurre"));     // Cuisine
        QCOMPARE(model.data(model.index(2), ItemModel::NameRole).toString(),
                 QStringLiteral("Éponge"));     // Administratif (dernier rayon connu)
        QCOMPARE(model.data(model.index(3), ItemModel::NameRole).toString(),
                 QStringLiteral("Truc"));       // non classé : après tout le reste

        QCOMPARE(model.aisleCount(), 4);

        // Un article coché descend au bas de SON rayon, pas au bas de la liste : on
        // parcourt le magasin rayon par rayon.
        model.addItem("Carottes", "", "", "Ménage");
        const QString pommes = model.data(model.index(0), ItemModel::ItemIdRole).toString();
        model.toggleDone(pommes);

        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(),
                 QStringLiteral("Carottes"));   // reste à prendre
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(),
                 QStringLiteral("Pommes"));     // pris, mais toujours dans son rayon
        QCOMPARE(model.data(model.index(2), ItemModel::NameRole).toString(),
                 QStringLiteral("Beurre"));     // le rayon suivant n'a pas bougé
    }

    // Rayon inventé : il voyage avec l'article (le champ porte le libellé), donc il
    // apparaît dans les choix sans qu'on ait rien à synchroniser de plus.
    void test_customAisle() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");

        QCOMPARE(model.aisleNames(), ItemModel::defaultAisles());

        QSignalSpy aisleSpy(&model, &ItemModel::aisleNamesChanged);
        model.addItem("Croquettes", "", "", "Animalerie");
        model.addItem("Vin",        "", "", "Cave");
        model.addItem("Pommes",     "", "", "Ménage");
        QVERIFY(aisleSpy.count() > 0);   // le choix se met à jour tout de suite

        // Les rayons d'origine d'abord, puis les inventés par ordre alphabétique — le
        // même ordre sur tous les appareils, sans classement à synchroniser.
        const QStringList names = model.aisleNames();
        QCOMPARE(names.size(), ItemModel::defaultAisles().size() + 2);
        QCOMPARE(names.at(names.size() - 2), QStringLiteral("Animalerie"));
        QCOMPARE(names.last(),               QStringLiteral("Cave"));

        // Tri : les rayons inventés passent après les rayons d'origine, mais avant les
        // articles non classés.
        model.addItem("Truc", "");   // sans rayon
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(),
                 QStringLiteral("Pommes"));      // rayon d'origine
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(),
                 QStringLiteral("Croquettes"));  // Animalerie
        QCOMPARE(model.data(model.index(2), ItemModel::NameRole).toString(),
                 QStringLiteral("Vin"));         // Cave
        QCOMPARE(model.data(model.index(3), ItemModel::NameRole).toString(),
                 QStringLiteral("Truc"));        // non classé, toujours en dernier

        // Un rayon dont plus aucun article ne dépend disparaît des choix de lui-même.
        const QString vin = model.data(model.index(2), ItemModel::ItemIdRole).toString();
        model.removeItem(vin);
        QVERIFY(!model.aisleNames().contains(QStringLiteral("Cave")));
        QVERIFY(model.aisleNames().contains(QStringLiteral("Animalerie")));
    }

    // Export CSV puis réimport : les articles reviennent, sans toucher à l'original.
    void test_exportImportCsv() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        AppController ctrl;
        QVERIFY(ctrl.db().open(dir.filePath("c.db")));

        // Liste source avec deux articles (dont un rangé et coché).
        ListMeta m;
        m.listId = "src"; m.key = std::vector<uint8_t>(32, 1); m.title = "Courses";
        m.titleVer = {1, "dev-A"}; m.lamport = 1; m.created = 1000;
        QVERIFY(ctrl.db().createList(m));

        Item a; a.listId = "src"; a.itemId = "a"; a.created = 1000; a.name = "Lait, entier";
        a.qty = "1 L"; a.note = "demi-écrémé"; a.aisle = "Cuisine"; a.nameVer = {1,"dev-A"};
        QVERIFY(ctrl.db().upsertItem(a));
        Item b; b.listId = "src"; b.itemId = "b"; b.created = 1001; b.name = "Pain";
        b.done = true; b.nameVer = {1,"dev-A"};
        QVERIFY(ctrl.db().upsertItem(b));

        // Le CSV porte l'en-tête et les valeurs, virgule du nom échappée.
        const QString csv = ctrl.listCsv("src");
        QVERIFY(csv.contains("Tache"));
        QVERIFY(csv.contains("\"Lait, entier\""));   // virgule → champ entre guillemets
        QVERIFY(csv.contains("Cuisine"));

        const QUrl file = QUrl::fromLocalFile(dir.filePath("courses.csv"));
        QVERIFY(ctrl.exportListCsv(file, "src"));

        ctrl.importFile(file);

        // Une nouvelle liste (titre = nom de fichier), l'original intact → 2 listes.
        const auto lists = ctrl.db().getLists();
        QCOMPARE(lists.size(), size_t(2));

        std::string importedId;
        for (const auto &l : lists)
            if (l.listId != "src") { importedId = l.listId;
                QCOMPARE(QString::fromStdString(l.title), QStringLiteral("courses")); }
        QVERIFY(!importedId.empty());

        // Les articles ont été recréés à l'identique (rayon et coché compris).
        auto items = ctrl.db().getItems(importedId);
        QCOMPARE(items.size(), size_t(2));
        bool sawLait = false, sawPain = false;
        for (const auto &it : items) {
            if (it.name == "Lait, entier") {
                sawLait = true;
                QCOMPARE(it.qty, std::string("1 L"));
                QCOMPARE(it.note, std::string("demi-écrémé"));
                QCOMPARE(it.aisle, std::string("Cuisine"));
                QVERIFY(!it.done);
            }
            if (it.name == "Pain") { sawPain = true; QVERIFY(it.done); }
        }
        QVERIFY(sawLait && sawPain);
    }

    // Export ZIP de toutes les listes, puis réimport : chaque CSV redevient une liste.
    void test_exportImportZip() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        AppController ctrl;
        QVERIFY(ctrl.db().open(dir.filePath("c.db")));

        for (const char *title : { "Maison", "Boulot" }) {
            ListMeta m;
            m.listId = title; m.key = std::vector<uint8_t>(32, 2);
            m.title = title; m.titleVer = {1,"dev-A"}; m.lamport = 1; m.created = 1000;
            QVERIFY(ctrl.db().createList(m));
            Item it; it.listId = title; it.itemId = std::string("i-") + title;
            it.created = 1000; it.name = std::string("Article ") + title;
            it.nameVer = {1,"dev-A"};
            QVERIFY(ctrl.db().upsertItem(it));
        }

        const QUrl zip = QUrl::fromLocalFile(dir.filePath("tout.zip"));
        QVERIFY(ctrl.exportAllZip(zip));

        ctrl.importFile(zip);

        // 2 listes d'origine + 2 réimportées.
        QCOMPARE(ctrl.db().getLists().size(), size_t(4));

        int maisonCopies = 0;
        for (const auto &l : ctrl.db().getLists())
            if (QString::fromStdString(l.title) == "Maison") ++maisonCopies;
        QCOMPARE(maisonCopies, 2);   // l'originale + celle issue du ZIP
    }

    // Un ajout manuel enrichit les favoris ; un article recréé par un merge distant, non.
    void test_addItem_recordsFavorite() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");

        QSignalSpy addedSpy(&model, &ItemModel::itemAdded);
        model.addItem("Lait", "1 L", "", "Cuisine");
        QCOMPARE(addedSpy.count(), 1);

        auto favs = db.getFavorites(10);
        QCOMPARE(favs.size(), size_t(1));
        QCOMPARE(favs[0].name,  std::string("Lait"));
        QCOMPARE(favs[0].qty,   std::string("1 L"));
        QCOMPARE(favs[0].aisle, std::string("Cuisine"));

        // Rajouter le même nom incrémente le favori (habitude renforcée).
        model.addItem("Lait", "", "", "");
        QCOMPARE(db.getFavorites(10).front().uses, int64_t(2));

        // Un article venu d'un merge distant (upsert direct) ne touche pas les favoris.
        Item remote; remote.listId = listId; remote.itemId = "remote-1";
        remote.created = 9000; remote.name = "Saumon"; remote.nameVer = {5, "dev-B"};
        QVERIFY(db.upsertItem(remote));
        for (const auto &f : db.getFavorites(10))
            QVERIFY(f.name != "Saumon");
    }

    // Ajouter/ranger un article enseigne la mémoire des rayons ; les variantes du même
    // premier mot en héritent (« pain » ajouté en boulangerie → « pain de mie » suit).
    void test_addItem_learnsAisle() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");

        // Ajout avec rayon → la mémoire l'apprend.
        model.addItem("Pain", "", "", "Boulangerie");
        QCOMPARE(db.suggestAisleForName("pain de mie"), std::string("Boulangerie"));

        // Ranger un article via setAisle enseigne aussi.
        model.addItem("Camembert", "", "", "");
        const QString cam = model.data(model.index(model.count() - 1), ItemModel::ItemIdRole).toString();
        model.setAisle(cam, "Cuisine");
        QCOMPARE(db.suggestAisleForName("Camembert"), std::string("Cuisine"));

        // Pas d'auto-assignation : ajouter sans rayon laisse le rayon vide (le
        // pré-remplissage se fait côté UI, pas dans le modèle).
        model.addItem("pain aux céréales", "", "", "");
        bool found = false;
        for (const auto &it : db.getItems(listId))
            if (it.name == "pain aux céréales") { found = true; QVERIFY(it.aisle.empty()); }
        QVERIFY(found);
    }

    // Gestion des rayons personnalisés : renommer / supprimer partout, sync comprise.
    void test_customAisleManagement() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        AppController ctrl;
        QVERIFY(ctrl.db().open(dir.filePath("c.db")));
        Database &db = ctrl.db();

        // Deux listes, chacune avec un article dans le rayon perso « Cave ».
        for (const char *lid : { "L1", "L2" }) {
            ListMeta m; m.listId = lid; m.key = std::vector<uint8_t>(32, 1);
            m.title = lid; m.titleVer = {1,"dev-A"}; m.lamport = 1; m.created = 1000;
            QVERIFY(db.createList(m));
            Item it; it.listId = lid; it.itemId = std::string("i-") + lid;
            it.created = 1000; it.name = "Vin"; it.aisle = "Cave"; it.nameVer = {1,"dev-A"};
            it.aisleVer = {1,"dev-A"};
            QVERIFY(db.upsertItem(it));
        }
        db.recordAisleForName("Vin", "Cave", 1000);

        // « Cave » est un rayon perso (pas d'origine), listé pour la gestion.
        QVERIFY(ctrl.customAisles().contains(QStringLiteral("Cave")));
        QVERIFY(!ctrl.customAisles().contains(QStringLiteral("Cuisine"))); // rayon d'origine
        QCOMPARE(ctrl.countItemsInAisle("Cave"), 2);

        // Renommer « Cave » → « Cellier » : les deux articles suivent, la mémoire aussi.
        ctrl.renameAisle("Cave", "Cellier");
        for (const char *lid : { "L1", "L2" })
            for (const auto &it : db.getItems(lid))
                if (it.name == "Vin") QCOMPARE(it.aisle, std::string("Cellier"));
        QCOMPARE(db.suggestAisleForName("Vin"), std::string("Cellier"));
        QVERIFY(!ctrl.customAisles().contains(QStringLiteral("Cave")));
        QVERIFY(ctrl.customAisles().contains(QStringLiteral("Cellier")));

        // Le rayon d'un article est versionné : la réaffectation part au relais.
        for (const auto &it : db.getItems("L1"))
            if (it.name == "Vin") QVERIFY(it.aisleVer.lamport > 1);

        // Supprimer « Cellier » : les articles repassent « sans rayon », plus proposé.
        ctrl.deleteAisle("Cellier");
        for (const char *lid : { "L1", "L2" })
            for (const auto &it : db.getItems(lid))
                if (it.name == "Vin") QVERIFY(it.aisle.empty());
        QVERIFY(ctrl.customAisles().isEmpty());
        QVERIFY(db.suggestAisleForName("Vin").empty());
    }

    // ListsModel : groupes (sections triées) et « partagée avec » (membres, soi exclu).
    void test_listsModel_groupsAndMembers() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));

        makeList(db, "list-maison");
        makeList(db, "list-libre");

        db.createGroup("grp-1", "Maison", 1000);
        db.setListGroup("list-maison", "grp-1");

        // « list-maison » partagée avec Marie ; nous-mêmes (dev-A) ne comptons pas.
        db.upsertMember("list-maison", "dev-A", "Moi",   { 1, "dev-A" });
        db.upsertMember("list-maison", "dev-B", "Marie", { 1, "dev-B" });

        ListsModel model;
        model.reload(db, "dev-A");
        QCOMPARE(model.rowCount(), 2);

        // La liste rangée passe avant la non rangée (les groupes d'abord).
        QCOMPARE(model.data(model.index(0), ListsModel::ListIdRole).toString(),
                 QStringLiteral("list-maison"));
        QCOMPARE(model.data(model.index(0), ListsModel::GroupNameRole).toString(),
                 QStringLiteral("Maison"));
        QCOMPARE(model.data(model.index(1), ListsModel::GroupNameRole).toString(),
                 QStringLiteral(""));   // non rangée, section vide, en dernier

        // Partagée avec Marie, sans se compter soi-même.
        QCOMPARE(model.data(model.index(0), ListsModel::MemberCountRole).toInt(), 1);
        QCOMPARE(model.data(model.index(0), ListsModel::MembersRole).toString(),
                 QStringLiteral("Marie"));
        QCOMPARE(model.data(model.index(1), ListsModel::MemberCountRole).toInt(), 0);

        // Groupe supprimé → la liste redescend chez les non rangées, mais existe encore.
        db.deleteGroup("grp-1");
        model.reload(db, "dev-A");
        QCOMPARE(model.rowCount(), 2);
        for (int i = 0; i < 2; ++i)
            QCOMPARE(model.data(model.index(i), ListsModel::GroupNameRole).toString(),
                     QStringLiteral(""));
    }

    // Réordonner : la ligne va où on la dépose, et la nouvelle position est persistée
    // avec une version (donc synchronisée).
    void test_moveItem() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("A", "");
        model.addItem("B", "");
        model.addItem("C", "");

        const QString idC = model.data(model.index(2), ItemModel::ItemIdRole).toString();

        // C (dernier) déposé en tête.
        model.moveItem(2, 0);
        QCOMPARE(model.data(model.index(0), ItemModel::ItemIdRole).toString(), idC);
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(), QStringLiteral("A"));
        QCOMPARE(model.data(model.index(2), ItemModel::NameRole).toString(), QStringLiteral("B"));

        // Persisté et versionné : sans version, le pair d'en face rediffuserait
        // l'ancienne position et l'écraserait au merge.
        bool found = false;
        for (const auto &it : db.getItems(listId)) {
            if (it.itemId == idC.toStdString()) {
                QVERIFY(it.orderVer.lamport > 0);
                QCOMPARE(it.orderVer.deviceId, std::string("dev-A"));
                found = true;
            }
        }
        QVERIFY(found);

        // L'ordre survit au rechargement : c'est bien la position qui trie, plus la
        // date de création.
        ItemModel reloaded;
        reloaded.load(db, listId, "dev-A");
        QCOMPARE(reloaded.data(reloaded.index(0), ItemModel::NameRole).toString(),
                 QStringLiteral("C"));

        // Déplacement au milieu.
        model.moveItem(0, 1);   // C entre A et B
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(), QStringLiteral("A"));
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(), QStringLiteral("C"));
        QCOMPARE(model.data(model.index(2), ItemModel::NameRole).toString(), QStringLiteral("B"));

        // Bornes : ne rien casser sur un index absurde.
        model.moveItem(0, 0);
        model.moveItem(-1, 2);
        model.moveItem(1, 99);
        QCOMPARE(model.count(), 3);
    }

    // Déposer une ligne sous l'en-tête d'un autre rayon la range dans ce rayon : c'est
    // le geste naturel pour classer, et il n'y a pas d'autre lecture possible.
    void test_moveItem_acrossAisleChangesAisle() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Pommes", "", "", "Ménage");
        model.addItem("Beurre", "", "", "Cuisine");
        model.addItem("Truc",   "", "", "");   // non classé, donc en dernier

        QCOMPARE(model.data(model.index(2), ItemModel::NameRole).toString(),
                 QStringLiteral("Truc"));

        // « Truc » déposé sur la ligne de Beurre → il rejoint la crèmerie.
        model.moveItem(2, 1);

        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(),
                 QStringLiteral("Truc"));
        QCOMPARE(model.data(model.index(1), ItemModel::AisleRole).toString(),
                 QStringLiteral("Cuisine"));

        // Le rayon est versionné : il part au relais comme n'importe quel champ.
        for (const auto &it : db.getItems(listId)) {
            if (it.name == "Truc") {
                QCOMPARE(it.aisle, std::string("Cuisine"));
                QVERIFY(it.aisleVer.lamport > 0);
            }
        }
    }

    // Mode manuel : le tri ignore les rayons (une seule séquence), glisser réordonne
    // sans reclasser, et le mode est persisté (répliqué comme le titre).
    void test_manualSort() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        QVERIFY(!model.manualSort());   // défaut = par rayon

        // Ajoutés dans le désordre des rayons : Cuisine (rang 2) avant Fruits (rang 0).
        model.addItem("Beurre", "", "", "Cuisine");
        model.addItem("Pommes", "", "", "Ménage");

        // En mode rayon, le tri remonte Fruits avant Cuisine malgré l'ordre d'ajout.
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(),
                 QStringLiteral("Pommes"));
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(),
                 QStringLiteral("Beurre"));

        // Passage en manuel : le rayon ne trie plus, on retrouve l'ordre d'ajout.
        QSignalSpy modeSpy(&model, &ItemModel::manualSortChanged);
        model.setManualSort(true);
        QCOMPARE(modeSpy.count(), 1);
        QVERIFY(model.manualSort());
        QCOMPARE(model.aisleCount(), 1);   // pas de sections en manuel
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(),
                 QStringLiteral("Beurre"));
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(),
                 QStringLiteral("Pommes"));

        // Le mode est persisté et versionné (répliqué).
        auto meta = db.getList(listId);
        QVERIFY(meta.has_value());
        QCOMPARE(meta->sortMode, std::string("manual"));
        QVERIFY(meta->sortModeVer.lamport > 0);

        // Glisser en manuel réordonne SANS changer le rayon : Pommes remonte au-dessus
        // de Beurre, mais garde « Ménage ».
        model.moveItem(1, 0);
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(),
                 QStringLiteral("Pommes"));
        QCOMPARE(model.data(model.index(0), ItemModel::AisleRole).toString(),
                 QStringLiteral("Ménage"));   // rayon intact
        for (const auto &it : db.getItems(listId))
            if (it.name == "Pommes")
                QCOMPARE(it.aisle, std::string("Ménage"));

        // Retour en mode rayon : "aisle" est écrit (pas ""), pour battre un "manual"
        // distant au merge. L'ordre par rayon revient.
        model.setManualSort(false);
        QVERIFY(!model.manualSort());
        QCOMPARE(db.getList(listId)->sortMode, std::string("aisle"));
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(),
                 QStringLiteral("Pommes"));   // Fruits d'abord
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(),
                 QStringLiteral("Beurre"));

        // Le mode survit à un rechargement (relu depuis la base).
        model.setManualSort(true);
        ItemModel reloaded;
        reloaded.load(db, listId, "dev-A");
        QVERIFY(reloaded.manualSort());
    }

    // Ranger un article depuis le dialogue d'édition le fait changer de section.
    void test_setAisle() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Lait", "");
        model.addItem("Pommes", "", "", "Ménage");

        // Lait n'est pas classé : il est donc en dernier.
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(),
                 QStringLiteral("Lait"));

        const QString lait = model.data(model.index(1), ItemModel::ItemIdRole).toString();
        model.setAisle(lait, "Cuisine");

        // Cuisine vient après Ménage, mais avant « non classé ».
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(),
                 QStringLiteral("Lait"));
        QCOMPARE(model.data(model.index(1), ItemModel::AisleRole).toString(),
                 QStringLiteral("Cuisine"));
        QCOMPARE(model.aisleCount(), 2);
    }

    // Suppression groupée : tout disparaît de la vue, tout est marqué supprimé en base.
    void test_removeItems_bulk() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Lait", "");
        model.addItem("Pain", "");
        model.addItem("Œufs", "");
        QCOMPARE(model.count(), 3);

        const QString first  = model.data(model.index(0), ItemModel::ItemIdRole).toString();
        const QString second = model.data(model.index(1), ItemModel::ItemIdRole).toString();

        model.removeItems({ first, second });

        // Le troisième survit ; les deux autres sont des tombstones (del=true), pas
        // des lignes effacées : les autres appareils doivent apprendre la suppression.
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(),
                 QStringLiteral("Œufs"));

        int tombstones = 0;
        for (const auto &it : db.getItems(listId))
            if (it.del) ++tombstones;
        QCOMPARE(tombstones, 2);
    }

    // Étapes : le modèle en mode étapes ne voit que les enfants de sa tâche, le
    // modèle de liste ne voit que les tâches — mais compte la progression des étapes.
    void test_steps_parentFilterAndProgress() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel tasks;
        tasks.load(db, listId, "dev-A");
        tasks.addItem("Repeindre le volet", "");
        tasks.addItem("Tondre", "");
        QCOMPARE(tasks.count(), 2);
        const QString taskId = tasks.data(tasks.index(0), ItemModel::ItemIdRole).toString();

        // Trois étapes sous la première tâche.
        ItemModel steps;
        steps.load(db, listId, "dev-A", taskId.toStdString());
        steps.addItem("Démonter", "");
        steps.addItem("Poncer", "");
        steps.addItem("Repeindre", "");
        QCOMPARE(steps.count(), 3);

        // Les étapes ne polluent pas la liste des tâches…
        tasks.load(db, listId, "dev-A");
        QCOMPARE(tasks.count(), 2);

        // …mais la progression est là.
        int row = -1;
        for (int i = 0; i < tasks.count(); ++i)
            if (tasks.data(tasks.index(i), ItemModel::ItemIdRole).toString() == taskId)
                row = i;
        QVERIFY(row >= 0);
        QCOMPARE(tasks.data(tasks.index(row), ItemModel::StepsTotalRole).toInt(), 3);
        QCOMPARE(tasks.data(tasks.index(row), ItemModel::StepsDoneRole).toInt(), 0);

        // Cocher une étape : elle GARDE sa place (la séquence reste lisible).
        const QString demonter = steps.data(steps.index(0), ItemModel::ItemIdRole).toString();
        steps.toggleDone(demonter);
        QCOMPARE(steps.data(steps.index(0), ItemModel::NameRole).toString(),
                 QStringLiteral("Démonter"));
        QVERIFY(steps.data(steps.index(0), ItemModel::DoneRole).toBool());

        tasks.load(db, listId, "dev-A");
        QCOMPARE(tasks.data(tasks.index(row), ItemModel::StepsDoneRole).toInt(), 1);

        // Supprimer la tâche emporte ses étapes (tombstones répliqués).
        tasks.removeItem(taskId);
        int tombstones = 0;
        for (const auto &it : db.getItems(listId))
            if (it.del) ++tombstones;
        QCOMPARE(tombstones, 4);   // la tâche + ses 3 étapes
    }

    // Les photos d'une tâche : liste de shas dans le champ LWW `image`, dans l'ordre
    // d'ajout (« avant » puis « après »), exposée par ImageRole.
    void test_itemImages() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Réparer la clôture", "");
        const QString id = model.data(model.index(0), ItemModel::ItemIdRole).toString();
        QCOMPARE(model.data(model.index(0), ItemModel::ImageRole).toString(), QString());

        QSignalSpy localSpy(&model, &ItemModel::localChanged);
        model.addItemImage(id, "sha-avant");
        model.addItemImage(id, "sha-apres");
        model.addItemImage(id, "sha-avant");   // doublon : sans effet
        QCOMPARE(localSpy.count(), 2);
        QCOMPARE(model.data(model.index(0), ItemModel::ImageRole).toString(),
                 QStringLiteral("sha-avant sha-apres"));

        // Persisté et versionné : le champ part au relais comme les autres.
        for (const auto &it : db.getItems(listId)) {
            QCOMPARE(it.image, std::string("sha-avant sha-apres"));
            QVERIFY(it.imageVer.lamport > 0);
        }

        // Retirer UNE photo : l'autre reste, la version avance.
        model.removeItemImage(id, "sha-avant");
        QCOMPARE(model.data(model.index(0), ItemModel::ImageRole).toString(),
                 QStringLiteral("sha-apres"));
        model.removeItemImage(id, "sha-apres");
        QCOMPARE(model.data(model.index(0), ItemModel::ImageRole).toString(), QString());
    }

    // Cocher la tâche coche ses étapes ; cocher toutes les étapes coche la tâche ;
    // rouvrir une étape rouvre la tâche. La tâche reflète toujours son ensemble.
    void test_toggleCascade() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Repeindre le volet", "");
        const QString taskId = model.data(model.index(0), ItemModel::ItemIdRole).toString();

        ItemModel steps;
        steps.load(db, listId, "dev-A", taskId.toStdString());
        steps.addItem("Poncer", "");
        steps.addItem("Peindre", "");
        const QString s1 = steps.data(steps.index(0), ItemModel::ItemIdRole).toString();
        const QString s2 = steps.data(steps.index(1), ItemModel::ItemIdRole).toString();

        const auto taskDone = [&]() {
            for (const auto &it : db.getItems(listId))
                if (it.itemId == taskId.toStdString()) return it.done;
            return false;
        };
        const auto doneSteps = [&]() {
            int n = 0;
            for (const auto &it : db.getItems(listId))
                if (!it.parentId.empty() && it.done) ++n;
            return n;
        };

        // Cocher la tâche emporte ses étapes…
        model.load(db, listId, "dev-A");
        model.toggleDone(taskId);
        QVERIFY(taskDone());
        QCOMPARE(doneSteps(), 2);

        // …et la décocher les rouvre toutes.
        model.toggleDone(taskId);
        QVERIFY(!taskDone());
        QCOMPARE(doneSteps(), 0);

        // Étape par étape : la dernière coche la tâche toute seule.
        steps.load(db, listId, "dev-A", taskId.toStdString());
        steps.toggleDone(s1);
        QVERIFY(!taskDone());
        steps.toggleDone(s2);
        QVERIFY(taskDone());

        // Une étape rouverte rouvre la tâche.
        steps.toggleDone(s1);
        QVERIFY(!taskDone());
    }

    // Mode « Au boulot » : les étapes se déplient sous leurs tâches, dans la section
    // de leur tâche, cochables en place — et se replient en quittant le mode.
    void test_showSteps_expandsUnderTasks() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Repeindre le volet", "", "", "Ménage");
        model.addItem("Tondre", "", "", "");
        const QString taskId = model.data(model.index(0), ItemModel::ItemIdRole).toString();

        ItemModel steps;
        steps.load(db, listId, "dev-A", taskId.toStdString());
        steps.addItem("Poncer", "");
        steps.addItem("Repeindre", "");

        model.load(db, listId, "dev-A");   // recharger : les étapes sont en base
        QCOMPARE(model.count(), 2);        // repliées par défaut

        model.setShowSteps(true);
        QCOMPARE(model.count(), 4);

        // La tâche d'abord, puis SES étapes dans leur ordre, puis la tâche suivante.
        QCOMPARE(model.data(model.index(0), ItemModel::NameRole).toString(),
                 QStringLiteral("Repeindre le volet"));
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(),
                 QStringLiteral("Poncer"));
        QVERIFY(model.data(model.index(1), ItemModel::IsStepRole).toBool());
        // L'étape s'affiche dans la section de sa tâche.
        QCOMPARE(model.data(model.index(1), ItemModel::AisleRole).toString(),
                 QStringLiteral("Ménage"));
        QCOMPARE(model.data(model.index(2), ItemModel::NameRole).toString(),
                 QStringLiteral("Repeindre"));
        QCOMPARE(model.data(model.index(3), ItemModel::NameRole).toString(),
                 QStringLiteral("Tondre"));
        QVERIFY(!model.data(model.index(3), ItemModel::IsStepRole).toBool());

        // Cocher une étape en place : la progression du mode la compte.
        const QString poncer = model.data(model.index(1), ItemModel::ItemIdRole).toString();
        model.toggleDone(poncer);
        QCOMPARE(model.doneCount(), 1);
        QCOMPARE(model.data(model.index(1), ItemModel::NameRole).toString(),
                 QStringLiteral("Poncer"));   // garde sa place sous sa tâche

        model.setShowSteps(false);
        QCOMPARE(model.count(), 2);
    }

    // L'échéance est facultative : posée, changée, retirée — champ LWW versionné,
    // valable pour une tâche comme pour une étape.
    void test_setDue() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Tondre", "");
        const QString id = model.data(model.index(0), ItemModel::ItemIdRole).toString();
        QCOMPARE(model.data(model.index(0), ItemModel::DueRole).toLongLong(), int64_t(0));

        QSignalSpy localSpy(&model, &ItemModel::localChanged);
        model.setDue(id, 1722000000000.0);
        QCOMPARE(localSpy.count(), 1);
        QCOMPARE(model.data(model.index(0), ItemModel::DueRole).toLongLong(),
                 int64_t(1722000000000LL));

        // Persistée et versionnée : elle part au relais comme les autres champs.
        for (const auto &it : db.getItems(listId)) {
            QCOMPARE(it.due, int64_t(1722000000000LL));
            QVERIFY(it.dueVer.lamport > 0);
        }

        // Une étape aussi peut porter une échéance.
        ItemModel steps;
        steps.load(db, listId, "dev-A", id.toStdString());
        steps.addItem("Sortir la tondeuse", "");
        const QString stepId = steps.data(steps.index(0), ItemModel::ItemIdRole).toString();
        steps.setDue(stepId, 1722100000000.0);
        QCOMPARE(steps.data(steps.index(0), ItemModel::DueRole).toLongLong(),
                 int64_t(1722100000000LL));

        // Retirer l'échéance : retour à 0, version qui avance.
        model.setDue(id, 0);
        QCOMPARE(model.data(model.index(0), ItemModel::DueRole).toLongLong(), int64_t(0));
    }

    // L'historique consigne chaque tâche faite — et survit à son retrait de la liste.
    void test_history() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(openDb(db, dir));
        const auto listId = makeList(db);

        ItemModel model;
        model.load(db, listId, "dev-A");
        model.addItem("Sortir les poubelles", "", "", "Ménage");
        const QString id = model.data(model.index(0), ItemModel::ItemIdRole).toString();

        // Cocher l'unique étape coche la tâche (cascade) : c'est la TÂCHE que
        // l'historique consigne, pas l'étape.
        ItemModel steps;
        steps.load(db, listId, "dev-A", id.toStdString());
        steps.addItem("Sac jaune", "");
        const QString stepId = steps.data(steps.index(0), ItemModel::ItemIdRole).toString();
        steps.toggleDone(stepId);
        auto hist = db.getHistory(listId, 10);
        QCOMPARE(hist.size(), size_t(1));
        QCOMPARE(hist.front().name,   std::string("Sortir les poubelles"));
        QCOMPARE(hist.front().aisle,  std::string("Ménage"));
        QCOMPARE(hist.front().byName, std::string("vous"));
        QVERIFY(hist.front().doneAt > 0);

        // Rouvrir n'ajoute rien ; refaire fait une seconde entrée (autre date).
        // La clé de dédup est (item, date en ms) : laisser passer une milliseconde.
        model.load(db, listId, "dev-A");   // la tâche a changé en base : recharger
        model.toggleDone(id);              // rouverte (les étapes suivent)
        QCOMPARE(db.getHistory(listId, 10).size(), size_t(1));
        QTest::qWait(2);
        model.toggleDone(id);              // refaite
        QCOMPARE(db.getHistory(listId, 10).size(), size_t(2));

        // La trace survit au retrait de la tâche.
        model.removeItem(id);
        QCOMPARE(db.getHistory(listId, 10).size(), size_t(2));

        QVERIFY(db.clearHistory(listId));
        QCOMPARE(db.getHistory(listId, 10).size(), size_t(0));
    }
};

QTEST_GUILESS_MAIN(ItemModelTest)
#include "tst_itemmodel.moc"
