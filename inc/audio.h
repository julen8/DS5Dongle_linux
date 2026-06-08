#pragma once

#include <cstdint>
#include <functional>

using ReadCallback = std::function<size_t(int16_t*, size_t)>;

void audioInit(ReadCallback readCallback);
void audioCleanup();
void audioLoop();
