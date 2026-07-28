#pragma once

#include <string>
#include <vector>

namespace core {

// CSV minimal conforme à RFC 4180 : champs séparés par des virgules, guillemets pour
// les valeurs contenant virgule / guillemet / saut de ligne, guillemets internes
// doublés. De quoi ouvrir l'export dans un tableur, et relire ce qu'on a écrit.

// Échappe un champ (ajoute des guillemets si nécessaire).
std::string csvEscape(const std::string& field);

// Sérialise des lignes en texte CSV (séparateur de ligne : "\r\n", comme la norme).
std::string csvWrite(const std::vector<std::vector<std::string>>& rows);

// Parse un texte CSV en lignes de champs. Tolère \n comme \r\n, et une dernière ligne
// sans saut final. Une ligne vide en fin de fichier est ignorée.
std::vector<std::vector<std::string>> csvParse(const std::string& text);

} // namespace core
