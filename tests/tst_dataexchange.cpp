// tst_dataexchange.cpp — CSV (RFC 4180) et ZIP « store ». Pur C++/STL, sans Qt.

#include "core/csv.h"
#include "core/zip.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_total = 0, g_passed = 0, g_failed = 0;

#define EXPECT_TRUE(expr)                                                      \
    do { ++g_total;                                                           \
        if (!(expr)) { ++g_failed;                                            \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); } \
        else ++g_passed; } while (0)
#define EXPECT_EQ(a, b) EXPECT_TRUE((a) == (b))

using namespace core;
using Rows = std::vector<std::vector<std::string>>;

static void test_csvEscape() {
    EXPECT_EQ(csvEscape("Lait"), "Lait");
    EXPECT_EQ(csvEscape("1 kg, bio"), "\"1 kg, bio\"");          // virgule
    EXPECT_EQ(csvEscape("dit \"frais\""), "\"dit \"\"frais\"\"\""); // guillemets doublés
    EXPECT_EQ(csvEscape("deux\nlignes"), "\"deux\nlignes\"");    // saut de ligne
}

static void test_csvRoundTrip() {
    Rows rows = {
        { "nom", "quantite", "description" },
        { "Lait", "1 L", "demi-écrémé" },
        { "Papier, toilette", "2", "dit \"épais\"" },
        { "Note\navec saut", "", "" },
    };
    const std::string csv = csvWrite(rows);
    const Rows back = csvParse(csv);
    EXPECT_EQ(back.size(), size_t(4));
    EXPECT_EQ(back, rows);   // aller-retour exact, virgules/guillemets/sauts compris
}

static void test_csvParseTolerant() {
    // \n seul (pas \r\n), et pas de saut final : doit rester lisible.
    const Rows r = csvParse("Lait,1L\nPain,");
    EXPECT_EQ(r.size(), size_t(2));
    EXPECT_EQ(r[0][0], "Lait");
    EXPECT_EQ(r[1][0], "Pain");
    EXPECT_EQ(r[1][1], "");   // champ vide final préservé

    // Un fichier à une colonne (juste des noms) reste exploitable.
    const Rows one = csvParse("Bananes\nŒufs\nBeurre\n");
    EXPECT_EQ(one.size(), size_t(3));
    EXPECT_EQ(one[2][0], "Beurre");

    // Ligne vide finale ignorée, mais un champ vide au milieu conservé.
    const Rows mid = csvParse("a,,c\r\n\r\n");
    EXPECT_EQ(mid.size(), size_t(1));
    EXPECT_EQ(mid[0].size(), size_t(3));
    EXPECT_EQ(mid[0][1], "");
}

static void test_crc32() {
    // Vecteur de référence : CRC-32 de "123456789" == 0xCBF43926.
    EXPECT_EQ(zipCrc32("123456789"), 0xCBF43926u);
    EXPECT_EQ(zipCrc32(""), 0u);
}

static void test_zipRoundTrip() {
    std::vector<ZipEntry> entries = {
        { "Courses maison.csv", "nom,quantite\r\nLait,1L\r\n" },
        { "Boulot.csv", "nom\r\nStylos\r\n" },
        { "vide.csv", "" },
    };
    const std::string archive = zipWrite(entries);

    // En-tête d'un vrai ZIP : signature "PK\3\4".
    EXPECT_TRUE(archive.size() > 4);
    EXPECT_EQ(archive.substr(0, 4), std::string("PK\x03\x04", 4));

    auto back = zipRead(archive);
    EXPECT_TRUE(back.has_value());
    EXPECT_EQ(back->size(), size_t(3));
    EXPECT_EQ((*back)[0].name, "Courses maison.csv");
    EXPECT_EQ((*back)[0].data, "nom,quantite\r\nLait,1L\r\n");
    EXPECT_EQ((*back)[1].name, "Boulot.csv");
    EXPECT_EQ((*back)[2].data, "");   // entrée vide préservée
}

static void test_zipRejectsGarbage() {
    EXPECT_TRUE(!zipRead("pas un zip du tout").has_value());
    EXPECT_TRUE(!zipRead("").has_value());
    // Un ZIP tronqué (juste l'en-tête local, pas de répertoire central) est rejeté.
    auto full = zipWrite({ { "a.csv", "x" } });
    EXPECT_TRUE(!zipRead(full.substr(0, 10)).has_value());
}

int main() {
    std::printf("=== tst_dataexchange ===\n");
    test_csvEscape();
    test_csvRoundTrip();
    test_csvParseTolerant();
    test_crc32();
    test_zipRoundTrip();
    test_zipRejectsGarbage();
    std::printf("\nResults: %d/%d passed, %d failed\n", g_passed, g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
