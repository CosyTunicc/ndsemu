#pragma once
#include <cstdio>

// Journal simple. Tu t'en serviras tout le temps (instructions, accès mémoire...).
#define LOG(fmt, ...) std::fprintf(stderr, "[ndsemu] " fmt "\n", ##__VA_ARGS__)
