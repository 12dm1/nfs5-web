#pragma once
#include <cstdio>

// AUTO1 parallel test instances: DX11Z_OUT is the directory for every file
// the wrapper writes, DX11Z_INI the INI it reads; unset = next to PORSCHE.EXE.
bool WrapperOutPath(char* out, size_t size, const char* name);
bool WrapperIniPath(char* out, size_t size);

void LogInit();
void LogClose();
// Log is buffered (a writer thread puts the lines on disk every ~100 ms);
// LogFlush writes them now (crash reports).
void LogFlush();
void Log(const char* fmt, ...);
void LogGameTrackTable(const char* where);
void LogTransition(const char* fmt, ...);
