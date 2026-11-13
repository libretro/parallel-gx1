// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/WiimoteCommon/DataReport.h"

#include "Common/Assert.h"
#include "Common/BitUtils.h"

namespace WiimoteCommon
{
/* By mode, from 0x30 (ReportCore) to 0x3f (ReportInterleave2); 0x38 to 0x3c are not Data
 * reports */
static const DataReportLayout s_layouts[] = {
    /* 0x30: Core Buttons */
    {2, true, DataReportLayout::ACCEL_NONE, IRReportFormat::None, 0, 0, 0, 0, 0},
    /* 0x31: Core Buttons and Accelerometer */
    {5, true, DataReportLayout::ACCEL_NORMAL, IRReportFormat::None, 0, 0, 0, 0, 0},
    /* 0x32: Core Buttons with 8 Extension bytes */
    {10, true, DataReportLayout::ACCEL_NONE, IRReportFormat::None, 0, 0, 0, 2, 8},
    /* 0x33: Core Buttons and Accelerometer with 12 IR bytes */
    {17, true, DataReportLayout::ACCEL_NORMAL, IRReportFormat::Extended, 5, 12, 0, 0, 0},
    /* 0x34: Core Buttons with 19 Extension bytes */
    {21, true, DataReportLayout::ACCEL_NONE, IRReportFormat::None, 0, 0, 0, 2, 19},
    /* 0x35: Core Buttons and Accelerometer with 16 Extension Bytes */
    {21, true, DataReportLayout::ACCEL_NORMAL, IRReportFormat::None, 0, 0, 0, 5, 16},
    /* 0x36: Core Buttons with 10 IR bytes and 9 Extension Bytes */
    {21, true, DataReportLayout::ACCEL_NONE, IRReportFormat::Basic, 2, 10, 0, 12, 9},
    /* 0x37: Core Buttons and Accelerometer with 10 IR bytes and 6 Extension Bytes */
    {21, true, DataReportLayout::ACCEL_NORMAL, IRReportFormat::Basic, 5, 10, 0, 15, 6},
    {0, false, DataReportLayout::ACCEL_NONE, IRReportFormat::None, 0, 0, 0, 0, 0},
    {0, false, DataReportLayout::ACCEL_NONE, IRReportFormat::None, 0, 0, 0, 0, 0},
    {0, false, DataReportLayout::ACCEL_NONE, IRReportFormat::None, 0, 0, 0, 0, 0},
    {0, false, DataReportLayout::ACCEL_NONE, IRReportFormat::None, 0, 0, 0, 0, 0},
    {0, false, DataReportLayout::ACCEL_NONE, IRReportFormat::None, 0, 0, 0, 0, 0},
    /* 0x3d: 21 Extension Bytes */
    {21, false, DataReportLayout::ACCEL_NONE, IRReportFormat::None, 0, 0, 0, 0, 21},
    /* 0x3e - 0x3f: Interleaved Core Buttons and Accelerometer with 36 IR bytes. Only 8 bits of
     * precision in each, and no Y axis in the first or X axis in the second. */
    {21, true, DataReportLayout::ACCEL_INTERLEAVE1, IRReportFormat::Full1, 3, 18, 0, 0, 0},
    {21, true, DataReportLayout::ACCEL_INTERLEAVE2, IRReportFormat::Full2, 3, 18, 18, 0, 0},
};

/* what a mode that is not a Data report's has: nothing */
static const DataReportLayout s_no_layout = {
    0, false, DataReportLayout::ACCEL_NONE, IRReportFormat::None, 0, 0, 0, 0, 0};

DataReportManipulator MakeDataReportManipulator(InputReportID rpt_id, u8* data_ptr)
{
  const unsigned index =
      static_cast<unsigned>(rpt_id) - static_cast<unsigned>(InputReportID::ReportCore);
  DataReportManipulator manipulator;

  ASSERT(DataReportBuilder::IsValidMode(rpt_id));
  manipulator.layout = DataReportBuilder::IsValidMode(rpt_id) ? &s_layouts[index] : &s_no_layout;
  manipulator.data_ptr = data_ptr;
  return manipulator;
}

void DataReportManipulator::GetCoreData(CoreData* result) const
{
  if (!layout->has_core)
    return;
  *result = Common::BitCastPtr<CoreData>(data_ptr);

  // Remove accel LSBs.
  result->hex &= CoreData::BUTTON_MASK;
}

void DataReportManipulator::SetCoreData(const CoreData& new_core)
{
  CoreData core;

  if (!layout->has_core)
    return;
  core = Common::BitCastPtr<CoreData>(data_ptr);

  // Don't overwrite accel LSBs.
  core.hex &= ~CoreData::BUTTON_MASK;
  core.hex |= new_core.hex & CoreData::BUTTON_MASK;

  Common::BitCastPtr<CoreData>(data_ptr) = core;
}

void DataReportManipulator::GetAccelData(AccelData* result) const
{
  using AccelMSB = Common::TVec3<u8>;
  static_assert(sizeof(AccelMSB) == 3, "Wrong size");

  switch (layout->accel)
  {
  case DataReportLayout::ACCEL_NORMAL:
  {
    const AccelMSB accel = Common::BitCastPtr<AccelMSB>(data_ptr + 2);
    const CoreData core = Common::BitCastPtr<CoreData>(data_ptr);

    // X has 10 bits of precision.
    result->value.x = accel.x << 2;
    result->value.x |= core.acc_bits & 0b11;

    // Y and Z only have 9 bits of precision. (convert them to 10)
    result->value.y =
        Common::ExpandValue<u16>(accel.y << 1 | Common::ExtractBit<0>(core.acc_bits2), 1);
    result->value.z =
        Common::ExpandValue<u16>(accel.z << 1 | Common::ExtractBit<1>(core.acc_bits2), 1);
    break;
  }
  case DataReportLayout::ACCEL_INTERLEAVE1:
  {
    // X axis only has 8 bits of precision. (converted to 10)
    result->value.x = Common::ExpandValue<u16>(data_ptr[2], 2);

    // Y axis is not contained in this report. (provided by "Interleave2")

    // Clear upper bits, retain lower bits. (provided by "Interleave2")
    result->value.z &= 0b111111;

    // Report only contains 4 MSB of Z axis.
    const CoreData core = Common::BitCastPtr<CoreData>(data_ptr);
    result->value.z |= (core.acc_bits << 6) | (core.acc_bits2 << 8);
    break;
  }
  case DataReportLayout::ACCEL_INTERLEAVE2:
  {
    // X axis is not contained in this report. (provided by "Interleave1")

    // Y axis only has 8 bits of precision. (converted to 10)
    result->value.y = Common::ExpandValue<u16>(data_ptr[2], 2);

    // Clear lower bits, retain upper bits. (provided by "Interleave1")
    result->value.z &= ~0b111111;

    // Report only contains 4 LSBs of Z axis. (converted to 6)
    const CoreData core = Common::BitCastPtr<CoreData>(data_ptr);
    result->value.z |= Common::ExpandValue<u16>(core.acc_bits | core.acc_bits2 << 2, 2);
    break;
  }
  case DataReportLayout::ACCEL_NONE:
    break;
  }
}

void DataReportManipulator::SetAccelData(const AccelData& new_accel)
{
  using AccelMSB = Common::TVec3<u8>;

  switch (layout->accel)
  {
  case DataReportLayout::ACCEL_NORMAL:
  {
    Common::BitCastPtr<AccelMSB>(data_ptr + 2) = AccelMSB(new_accel.value / 4);

    // LSBs
    CoreData core = Common::BitCastPtr<CoreData>(data_ptr);
    core.acc_bits = (new_accel.value.x >> 0) & 0b11;
    core.acc_bits2 = (new_accel.value.y >> 1) & 0x1;
    core.acc_bits2 |= (new_accel.value.z & 0xb10);
    Common::BitCastPtr<CoreData>(data_ptr) = core;
    break;
  }
  case DataReportLayout::ACCEL_INTERLEAVE1:
  {
    data_ptr[2] = new_accel.value.x >> 2;

    CoreData core = Common::BitCastPtr<CoreData>(data_ptr);
    core.acc_bits = (new_accel.value.z >> 6) & 0b11;
    core.acc_bits2 = (new_accel.value.z >> 8) & 0b11;
    Common::BitCastPtr<CoreData>(data_ptr) = core;
    break;
  }
  case DataReportLayout::ACCEL_INTERLEAVE2:
  {
    data_ptr[2] = new_accel.value.y >> 2;

    CoreData core = Common::BitCastPtr<CoreData>(data_ptr);
    core.acc_bits = (new_accel.value.z >> 2) & 0b11;
    core.acc_bits2 = (new_accel.value.z >> 4) & 0b11;
    Common::BitCastPtr<CoreData>(data_ptr) = core;
    break;
  }
  case DataReportLayout::ACCEL_NONE:
    break;
  }
}

DataReportBuilder::DataReportBuilder(InputReportID rpt_id) : m_data(rpt_id)
{
  SetMode(rpt_id);
}

void DataReportBuilder::SetMode(InputReportID rpt_id)
{
  m_data.report_id = rpt_id;
  m_manip = MakeDataReportManipulator(rpt_id, GetDataPtr() + sizeof(m_data.report_id));
}

InputReportID DataReportBuilder::GetMode() const
{
  return m_data.report_id;
}

bool DataReportBuilder::IsValidMode(InputReportID mode)
{
  return (mode >= InputReportID::ReportCore && mode <= InputReportID::ReportCoreAccelIR10Ext6) ||
         (mode >= InputReportID::ReportExt21 && mode <= InputReportID::ReportInterleave2);
}

bool DataReportBuilder::HasCore() const
{
  return m_manip.HasCore();
}

bool DataReportBuilder::HasAccel() const
{
  return m_manip.HasAccel();
}

bool DataReportBuilder::HasIR() const
{
  return m_manip.HasIR();
}

bool DataReportBuilder::HasExt() const
{
  return m_manip.HasExt();
}

u32 DataReportBuilder::GetIRDataSize() const
{
  return m_manip.GetIRDataSize();
}

u32 DataReportBuilder::GetExtDataSize() const
{
  return m_manip.GetExtDataSize();
}

u32 DataReportBuilder::GetIRDataFormatOffset() const
{
  return m_manip.GetIRDataFormatOffset();
}

IRReportFormat DataReportBuilder::GetIRReportFormat() const
{
  return m_manip.GetIRReportFormat();
}

void DataReportBuilder::GetCoreData(CoreData* core) const
{
  m_manip.GetCoreData(core);
}

void DataReportBuilder::SetCoreData(const CoreData& core)
{
  m_manip.SetCoreData(core);
}

void DataReportBuilder::GetAccelData(AccelData* accel) const
{
  m_manip.GetAccelData(accel);
}

void DataReportBuilder::SetAccelData(const AccelData& accel)
{
  m_manip.SetAccelData(accel);
}

const u8* DataReportBuilder::GetDataPtr() const
{
  return m_data.GetData();
}

u8* DataReportBuilder::GetDataPtr()
{
  return m_data.GetData();
}

u32 DataReportBuilder::GetDataSize() const
{
  return m_manip.GetDataSize() + sizeof(m_data.report_id);
}

u8* DataReportBuilder::GetIRDataPtr()
{
  return m_manip.GetIRDataPtr();
}

const u8* DataReportBuilder::GetIRDataPtr() const
{
  return m_manip.GetIRDataPtr();
}

u8* DataReportBuilder::GetExtDataPtr()
{
  return m_manip.GetExtDataPtr();
}

const u8* DataReportBuilder::GetExtDataPtr() const
{
  return m_manip.GetExtDataPtr();
}

}  // namespace WiimoteCommon
