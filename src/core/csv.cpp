#include "csv.h"

namespace core {

std::string csvEscape(const std::string& field) {
    bool needQuote = false;
    for (char c : field) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { needQuote = true; break; }
    }
    if (!needQuote)
        return field;

    std::string out = "\"";
    for (char c : field) {
        if (c == '"') out += '"';   // guillemet interne doublé
        out += c;
    }
    out += '"';
    return out;
}

std::string csvWrite(const std::vector<std::vector<std::string>>& rows) {
    std::string out;
    for (const auto& row : rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i) out += ',';
            out += csvEscape(row[i]);
        }
        out += "\r\n";
    }
    return out;
}

std::vector<std::vector<std::string>> csvParse(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool inQuotes = false;
    bool fieldStarted = false;   // distingue une ligne vide d'une ligne à un champ vide

    auto endField = [&]() {
        row.push_back(field);
        field.clear();
        fieldStarted = false;
    };
    auto endRow = [&]() {
        endField();
        // Ligne entièrement vide (aucun champ démarré, un seul champ vide) : on la saute,
        // ce qui absorbe la ligne finale après le dernier \r\n.
        if (!(row.size() == 1 && row[0].empty()))
            rows.push_back(row);
        row.clear();
    };

    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];

        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';   // guillemet doublé → un guillemet littéral
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += c;
            }
            continue;
        }

        if (c == '"' && !fieldStarted) {
            inQuotes = true;
            fieldStarted = true;
        } else if (c == ',') {
            endField();
        } else if (c == '\n') {
            endRow();
        } else if (c == '\r') {
            // Fin de ligne \r\n : le \n suivant est traité par le cas ci-dessus ; un \r
            // isolé termine aussi la ligne.
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                endRow();
                ++i;
            } else {
                endRow();
            }
        } else {
            field += c;
            fieldStarted = true;
        }
    }

    // Dernier champ/ligne sans saut de ligne final.
    if (fieldStarted || !field.empty() || !row.empty())
        endRow();

    return rows;
}

} // namespace core
