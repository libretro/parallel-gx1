// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/Jit64Common/ConstantPool.h"

#include <cstring>
#include <memory>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

ConstantPool::ConstantPool()
{
  constant_pool_infos_init(&m_const_info, CONST_POOL_SIZE / ALIGNMENT);
}

ConstantPool::~ConstantPool()
{
  constant_pool_infos_free(&m_const_info);
}

void ConstantPool::Init(void* memory, size_t size)
{
  m_region = memory;
  m_region_size = size;
  Clear();
}

void ConstantPool::Clear()
{
  m_current_ptr = m_region;
  m_remaining_size = m_region_size;
  constant_pool_infos_clear(&m_const_info);
}

void ConstantPool::Shutdown()
{
  m_region = nullptr;
  m_region_size = 0;
  m_current_ptr = nullptr;
  m_remaining_size = 0;
  constant_pool_infos_clear(&m_const_info);
}

const void* ConstantPool::GetConstant(const void* value, size_t element_size, size_t num_elements,
                                      size_t index)
{
  const size_t value_size = element_size * num_elements;
  ConstantPoolInfo* const found = constant_pool_infos_get(&m_const_info, value);
  ASSERT_MSG(DYNA_REC, found, "Constant pool has no room to note a constant.");
  ConstantPoolInfo& info = *found;

  if (!info.m_location)
  {
    void* ptr = std::align(ALIGNMENT, value_size, m_current_ptr, m_remaining_size);
    ASSERT_MSG(DYNA_REC, ptr, "Constant pool has run out of space.");

    m_current_ptr = static_cast<u8*>(m_current_ptr) + value_size;
    m_remaining_size -= value_size;

    std::memcpy(ptr, value, value_size);
    info.m_location = ptr;
    info.m_size = value_size;
  }

  ASSERT_MSG(DYNA_REC, info.m_size == value_size, "Constant has incorrect size in constant pool.");
  u8* location = static_cast<u8*>(info.m_location);
  return location + element_size * index;
}
