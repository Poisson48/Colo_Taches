#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace core {

// Archive ZIP minimale, entrées NON compressées (méthode « store »). C'est un ZIP
// standard : n'importe quel outil (unzip, l'explorateur de fichiers d'Android, un
// tableur) l'ouvre. Suffisant pour du texte CSV, et sans dépendance ni compression
// à embarquer. La lecture n'accepte que « store » — c'est le format qu'on produit.

struct ZipEntry {
    std::string name;
    std::string data;
};

// Construit l'archive (octets bruts) à partir des entrées.
std::string zipWrite(const std::vector<ZipEntry>& entries);

// Lit une archive. nullopt si les octets ne forment pas un ZIP « store » exploitable.
std::optional<std::vector<ZipEntry>> zipRead(const std::string& bytes);

// CRC-32 (polynôme ZIP), exposé pour les tests.
uint32_t zipCrc32(const std::string& data);

} // namespace core
