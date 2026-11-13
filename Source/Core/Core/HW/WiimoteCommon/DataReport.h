// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "Common/CommonTypes.h"
#include "Common/Matrix.h"
#include "Core/HW/WiimoteCommon/WiimoteConstants.h"
#include "Core/HW/WiimoteCommon/WiimoteHid.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"

namespace WiimoteCommon
{
/* Where each part of a Wiimote "Data" report lies, by mode. */
struct DataReportLayout
{
  enum AccelKind : u8
  {
    ACCEL_NONE,
    ACCEL_NORMAL,      /* 8 MSBs per axis after the core, LSBs in the core's spare bits */
    ACCEL_INTERLEAVE1, /* X's 8 bits; Z's 4 MSBs in the core's spare bits */
    ACCEL_INTERLEAVE2  /* Y's 8 bits; Z's 4 LSBs in the core's spare bits */
  };

  u8 data_size; /* the report's bytes after its id */
  bool has_core;
  AccelKind accel;
  IRReportFormat ir_format;
  u8 ir_offset;
  u8 ir_size;
  u8 ir_format_offset;
  u8 ext_offset;
  u8 ext_size;
};

/* A Data report of one mode over borrowed bytes: what is not in the report reads as nothing,
 * and writing it does nothing. Made by value; nothing is allocated. */
class DataReportManipulator
{
public:
  using CoreData = ButtonData;

  bool HasCore() const { return layout->has_core; }
  bool HasAccel() const { return layout->accel != DataReportLayout::ACCEL_NONE; }
  bool HasIR() const { return layout->ir_size != 0; }
  bool HasExt() const { return layout->ext_size != 0; }

  void GetCoreData(CoreData*) const;
  void GetAccelData(AccelData*) const;

  void SetCoreData(const CoreData&);
  void SetAccelData(const AccelData&);

  u8* GetIRDataPtr() { return HasIR() ? data_ptr + layout->ir_offset : nullptr; }
  const u8* GetIRDataPtr() const { return HasIR() ? data_ptr + layout->ir_offset : nullptr; }
  u32 GetIRDataSize() const { return layout->ir_size; }
  u32 GetIRDataFormatOffset() const { return layout->ir_format_offset; }
  IRReportFormat GetIRReportFormat() const { return layout->ir_format; }

  u8* GetExtDataPtr() { return HasExt() ? data_ptr + layout->ext_offset : nullptr; }
  const u8* GetExtDataPtr() const { return HasExt() ? data_ptr + layout->ext_offset : nullptr; }
  u32 GetExtDataSize() const { return layout->ext_size; }

  u8* GetDataPtr() { return data_ptr; }
  const u8* GetDataPtr() const { return data_ptr; }

  u32 GetDataSize() const { return layout->data_size; }

  const DataReportLayout* layout;
  u8* data_ptr;
};

/* The manipulator for a mode over @data_ptr; a mode that is not a Data report's has nothing in
 * it */
DataReportManipulator MakeDataReportManipulator(InputReportID rpt_id, u8* data_ptr);

class DataReportBuilder
{
public:
  explicit DataReportBuilder(InputReportID rpt_id);

  using CoreData = ButtonData;

  void SetMode(InputReportID rpt_id);
  InputReportID GetMode() const;

  static bool IsValidMode(InputReportID rpt_id);

  bool HasCore() const;
  bool HasAccel() const;
  bool HasIR() const;
  bool HasExt() const;

  u32 GetIRDataSize() const;
  u32 GetExtDataSize() const;

  u32 GetIRDataFormatOffset() const;
  IRReportFormat GetIRReportFormat() const;

  void GetCoreData(CoreData*) const;
  void GetAccelData(AccelData*) const;

  void SetCoreData(const CoreData&);
  void SetAccelData(const AccelData&);

  u8* GetIRDataPtr();
  const u8* GetIRDataPtr() const;
  u8* GetExtDataPtr();
  const u8* GetExtDataPtr() const;

  u8* GetDataPtr();
  const u8* GetDataPtr() const;

  u32 GetDataSize() const;

  // The largest report is 0x3d (21 extension bytes).
  static constexpr int MAX_DATA_SIZE = 21;

private:
  TypedInputData<std::array<u8, MAX_DATA_SIZE>> m_data;

  DataReportManipulator m_manip;
};

}  // namespace WiimoteCommon
