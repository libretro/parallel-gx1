// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/DynamicLibrary.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Common
{
DynamicLibrary::DynamicLibrary() = default;

DynamicLibrary::DynamicLibrary(const char* filename)
{
  Open(filename);
}

DynamicLibrary::DynamicLibrary(void* handle)
{
  m_handle = handle;
}

DynamicLibrary::~DynamicLibrary()
{
  Close();
}

DynamicLibrary& DynamicLibrary::operator=(void* handle)
{
  m_handle = handle;
  return *this;
}

std::string DynamicLibrary::GetUnprefixedFilename(const char* filename)
{
#if defined(_WIN32)
  return std::string(filename) + ".dll";
#elif defined(__APPLE__)
  return std::string(filename) + ".dylib";
#else
  return std::string(filename) + ".so";
#endif
}

std::string DynamicLibrary::GetVersionedFilename(const char* libname, int major, int minor)
{
  char name[256];
#if defined(_WIN32)
  if (major >= 0 && minor >= 0)
    snprintf(name, sizeof(name), "%s-%d-%d.dll", libname, major, minor);
  else if (major >= 0)
    snprintf(name, sizeof(name), "%s-%d.dll", libname, major);
  else
    snprintf(name, sizeof(name), "%s.dll", libname);
#elif defined(__APPLE__)
  const char* prefix = std::strncmp(libname, "lib", 3) ? "lib" : "";
  if (major >= 0 && minor >= 0)
    snprintf(name, sizeof(name), "%s%s.%d.%d.dylib", prefix, libname, major, minor);
  else if (major >= 0)
    snprintf(name, sizeof(name), "%s%s.%d.dylib", prefix, libname, major);
  else
    snprintf(name, sizeof(name), "%s%s.dylib", prefix, libname);
#else
  const char* prefix = std::strncmp(libname, "lib", 3) ? "lib" : "";
  if (major >= 0 && minor >= 0)
    snprintf(name, sizeof(name), "%s%s.so.%d.%d", prefix, libname, major, minor);
  else if (major >= 0)
    snprintf(name, sizeof(name), "%s%s.so.%d", prefix, libname, major);
  else
    snprintf(name, sizeof(name), "%s%s.so", prefix, libname);
#endif
  return name;
}

bool DynamicLibrary::Open(const char* filename)
{
#ifdef _WIN32
  m_handle = reinterpret_cast<void*>(LoadLibraryA(filename));
#else
  m_handle = dlopen(filename, RTLD_NOW);
#endif
  return m_handle != nullptr;
}

void DynamicLibrary::Close()
{
  if (!IsOpen())
    return;

#ifdef _WIN32
  FreeLibrary(static_cast<HMODULE>(m_handle));
#else
  dlclose(m_handle);
#endif
  m_handle = nullptr;
}

void* DynamicLibrary::GetSymbolAddress(const char* name) const
{
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
  return reinterpret_cast<void*>(dlsym(m_handle, name));
#endif
}
}  // namespace Common
