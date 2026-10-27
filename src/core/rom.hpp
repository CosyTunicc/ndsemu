#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Lit un fichier entier en mémoire. Retourne false en cas d'erreur.
bool loadFile(const std::string& path, std::vector<uint8_t>& out);
bool loadFile(const std::filesystem::path& path, std::vector<uint8_t>& out);   // chemins avec accents sous Windows
