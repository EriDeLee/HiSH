#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hish {
struct SessionInfo { int id; int pid; bool pty; int ptyError; };
struct Output { std::vector<uint8_t> bytes; bool running; bool rawInput = false; bool echoInput = true; bool convertEol = false; std::vector<uint8_t> controlChars; uint32_t localFlags = 0; };
SessionInfo Start(const std::string& root, int columns, int rows, const std::string& mode);
Output Read(int id);
size_t Write(int id, const uint8_t* bytes, size_t size);
void Resize(int id, int columns, int rows);
void Close(int id);
}
