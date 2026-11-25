// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include <queues/retro_deque.h>
#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Core/IOS/USB/Common.h"

namespace IOS::HLE::USB
{
constexpr u8 INFINITY_NUM_BLOCKS = 0x14;
constexpr u8 INFINITY_BLOCK_SIZE = 0x10;

struct InfinityFigure final
{
  void Save();

  File::IOFile inf_file;
  u8 data[INFINITY_NUM_BLOCKS * INFINITY_BLOCK_SIZE]{};
  bool present = false;
  u8 order_added = 255;
};

// A response owed to the next interrupt transfer, and the transfers waiting for one
struct InfinityQuery
{
  u8 bytes[32];
};
RETRO_DEQUE_DEFINE(infinity_queries, InfinityQuery);
RETRO_DEQUE_DEFINE(infinity_waiting, IntrMessage*);

class InfinityUSB final : public Device
{
public:
  InfinityUSB();
  ~InfinityUSB() override;
  DeviceDescriptor GetDeviceDescriptor() const override;
  std::vector<ConfigDescriptor> GetConfigurations() const override;
  std::vector<InterfaceDescriptor> GetInterfaces(u8 config) const override;
  std::vector<EndpointDescriptor> GetEndpoints(u8 config, u8 iface, u8 alt) const override;
  bool Attach() override;
  bool AttachAndChangeInterface(u8 iface) override;
  int CancelTransfer(u8 endpoint) override;
  int ChangeInterface(u8 iface) override;
  int GetNumberOfAltSettings(u8 iface) override;
  int SetAltSetting(u8 alt_setting) override;
  int SubmitTransfer(CtrlMessage* message) override;
  int SubmitTransfer(BulkMessage* message) override;
  int SubmitTransfer(IntrMessage* message) override;
  int SubmitTransfer(IsoMessage* message) override;

private:
  // takes command
  // data: 32 bytes
  void ScheduleTransfer(TransferCommand* command, const u8* data, u64 expected_time_us);

  u16 m_vid = 0;
  u16 m_pid = 0;
  u8 m_active_interface = 0;
  bool m_device_attached = false;
  DeviceDescriptor m_device_descriptor;
  std::vector<ConfigDescriptor> m_config_descriptor;
  std::vector<InterfaceDescriptor> m_interface_descriptor;
  std::vector<EndpointDescriptor> m_endpoint_descriptor;
  // made with the base: a poll allocates nothing; the transfers waiting are owned here
  infinity_queries_t m_queries;
  infinity_waiting_t m_response_list;
};

enum class FigureUIPosition : u8
{
  HexagonDiscOne = 0,
  HexagonDiscTwo = 1,
  HexagonDiscThree = 2,
  PlayerOne = 3,
  P1AbilityOne = 4,
  P1AbilityTwo = 5,
  PlayerTwo = 6,
  P2AbilityOne = 7,
  P2AbilityTwo = 8
};

enum class FigureBasePosition : u8
{
  Unknown = 0,
  HexagonSlot = 1,
  PlayerOneSlot = 2,
  PlayerTwoSlot = 3
};

class InfinityBase final
{
public:
  InfinityBase();
  ~InfinityBase();
  InfinityBase(const InfinityBase&) = delete;
  InfinityBase& operator=(const InfinityBase&) = delete;

  bool HasFigureBeenAddedRemoved() const;
  void PopAddedRemovedResponse(u8* reply_buf);
  void GetBlankResponse(u8 sequence, u8* reply_buf);
  void GetPresentFigures(u8 sequence, u8* reply_buf);
  void GetFigureIdentifier(u8 fig_num, u8 sequence, u8* reply_buf);
  void QueryBlock(u8 fig_num, u8 block, u8* reply_buf, u8 sequence);
  void WriteBlock(u8 fig_num, u8 block, const u8* to_write_buf, u8* reply_buf, u8 sequence);
  void DescrambleAndSeed(u8* buf, u8 sequence, u8* reply_buf);
  void GetNextAndScramble(u8 sequence, u8* reply_buf);
  // Returns Infinity Figure name based on data from in_file param
  // buf: INFINITY_NUM_BLOCKS * INFINITY_BLOCK_SIZE bytes
  std::string LoadFigure(const u8* buf, File::IOFile in_file, FigureUIPosition position);
  std::string FindFigure(u32 character) const;

protected:
  InfinityFigure m_figures[9];

private:
  InfinityFigure& GetFigureByOrder(u8 order_added);
  // key: 16 bytes
  void GenerateInfinityFigureKey(const std::vector<u8>& sha1_data, u8* key);
  FigureBasePosition DeriveFigurePosition(FigureUIPosition position);
  void GenerateSeed(u32 seed);
  u32 GetNext();
  u64 Scramble(u32 num_to_scramble, u32 garbage);
  u32 Descramble(u64 num_to_descramble);
  u8 GenerateChecksum(const u8* data, int num_of_bytes) const;

  // These 4 variables are state variables used during the seeding and use of the random number
  // generator.
  u32 m_random_a;
  u32 m_random_b;
  u32 m_random_c;
  u32 m_random_d;

  u8 m_figure_order = 0;
  infinity_queries_t m_figure_added_removed_response;
};
}  // namespace IOS::HLE::USB
