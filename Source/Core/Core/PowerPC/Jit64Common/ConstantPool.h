// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CodePtrMap.h"

struct ConstantPoolInfo
{
  void* m_location;  // null while not copied
  size_t m_size;
};

CODE_PTR_MAP_DEFINE(constant_pool_infos, ConstantPoolInfo);

// Constants are copied into this pool so that they live at a memory location
// that is close to the code that references it. This ensures that the 32-bit
// limitation on RIP addressing is not an issue.
class ConstantPool
{
public:
  static constexpr size_t CONST_POOL_SIZE = 1024 * 32;
  static constexpr size_t ALIGNMENT = 16;

  ConstantPool();
  ConstantPool(const ConstantPool&) = delete;
  ConstantPool& operator=(const ConstantPool&) = delete;
  ~ConstantPool();

  void Init(void* memory, size_t size);
  void Clear();
  void Shutdown();

  // Copies the value into the pool if it doesn't exist. Returns a pointer
  // to existing values if they were already copied. Pointer equality is
  // used to determine if two constants are the same.
  const void* GetConstant(const void* value, size_t element_size, size_t num_elements,
                          size_t index);

private:
  void* m_region = nullptr;
  size_t m_region_size = 0;
  void* m_current_ptr = nullptr;
  size_t m_remaining_size = 0;
  // made with the pool, as large as a full pool needs (each constant takes ALIGNMENT bytes at
  // least), so copying one in allocates nothing
  constant_pool_infos_t m_const_info{};
};
