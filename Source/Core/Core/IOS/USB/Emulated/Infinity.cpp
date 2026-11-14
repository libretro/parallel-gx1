// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/USB/Emulated/Infinity.h"

#include <array>
#include <bit>
#include <vector>

#include <crypto/crypto.h>

#include "Common/BitUtils.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Logging/Log.h"
#include "Common/Random.h"
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE::USB
{
// Information taken from https://disneyinfinity.fandom.com/wiki/Model_Numbers
const std::array<std::pair<const char*, const u32>, 104> list_infinity_figures = {
    {{"The Incredibles - Mr. Incredible", 0x0F4241},
     {"Monsters University - Sulley", 0x0F4242},
     {"Pirates of the Caribbean - Jack Sparrow", 0x0F4243},
     {"The Lone Ranger - The Lone Ranger", 0x0F4244},
     {"The Lone Ranger - Tonto", 0x0F4245},
     {"Cars - Lightning McQueen", 0x0F4246},
     {"Cars - Holley Shiftwell", 0x0F4247},
     {"Toy Story - Buzz Lightyear", 0x0F4248},
     {"Toy Story - Jessie", 0x0F4249},
     {"Monsters University - Mike Wazowski", 0x0F424A},
     {"The Incredibles - Mrs. Incredible", 0x0F424B},
     {"Pirates of the Caribbean - Barbossa", 0x0F424C},
     {"Pirates of the Caribbean - Davy Jones", 0x0F424D},
     {"Monsters University - Randy", 0x0F424E},
     {"The Incredibles - Syndrome", 0x0F424F},
     {"Toy Story - Woody", 0x0F4250},
     {"Cars - Mater", 0x0F4251},
     {"The Incredibles - Dash", 0x0F4252},
     {"The Incredibles - Violet", 0x0F4253},
     {"Cars - Francesco Bernoulli", 0x0F4254},
     {"Fantasia - Sorcerer's Apprentice Mickey", 0x0F4255},
     {"The Nightmare Before Christmas - Jack Skellington", 0x0F4256},
     {"Tangled - Rapunzel", 0x0F4257},
     {"Frozen - Anna", 0x0F4258},
     {"Frozen - Elsa", 0x0F4259},
     {"Phineas and Ferb - Phineas Flynn", 0x0F425A},
     {"Phineas and Ferb - Agent P", 0x0F425B},
     {"Wreck-It Ralph - Wreck-It Ralph", 0x0F425C},
     {"Wreck-It Ralph - Vanellope von Schweetz", 0x0F425D},
     {"The Incredibles - Mr. Incredible (Crystal)", 0x0F425E},
     {"Pirates of the Caribbean - Jack Sparrow (Crystal)", 0x0F425F},
     {"Monsters University - Sulley (Crystal)", 0x0F4260},
     {"Cars - Lightning McQueen (Crystal)", 0x0F4261},
     {"The Lone Ranger - The Lone Ranger (Crystal)", 0x0F4262},
     {"Toy Story - Buzz Lightyear (Crystal)", 0x0F4263},
     {"Phineas and Ferb - Agent P (Crystal)", 0x0F4264},
     {"Fantasia - Sorcerer's Apprentice Mickey (Crystal)", 0x0F4265},
     {"Toy Story - Buzz Lightyear (Glowing)", 0x0F4266},
     {"The Incredibles - Pirates of the Caribbean - Monsters University Play Set", 0x1E8481},
     {"The Lone Ranger Play Set", 0x1E8482},
     {"Cars Play Set", 0x1E8483},
     {"Toy Story in Space Play Set", 0x1E8484},
     {"Bolt - Bolt's Super Strength - Ability", 0x2DC6C3},
     {"Wreck-it Ralph - Ralph's Power of Destruction - Ability", 0x2DC6C4},
     {"Fantasia - Chernabog's Power - Ability", 0x2DC6C5},
     {"Cars - C.H.R.O.M.E. Damage Increaser - Ability", 0x2DC6C6},
     {"Phineas and Ferb - Dr. Doofenshmirtz's Damage-Inator! - Ability", 0x2DC6C7},
     {"Frankenweenie - Electro-Charge - Ability", 0x2DC6C8},
     {"Wreck-It Ralph - Fix-It Felix's Repair Power - Ability", 0x2DC6C9},
     {"Tangled - Rapunzel's Healing - Ability", 0x2DC6CA},
     {"Cars - C.H.R.O.M.E. Armor Shield - Ability", 0x2DC6CB},
     {"Toy Story - Star Command Shield - Ability", 0x2DC6CC},
     {"The Incredibles - Violet's Force Field - Ability", 0x2DC6CD},
     {"Pirates of the Caribbean - Pieces of Eight - Ability", 0x2DC6CE},
     {"DuckTales - Scrooge McDuck's Lucky Dime - Ability", 0x2DC6CF},
     {"TRON - User Control Disc - Ability", 0x2DC6D0},
     {"Fantasia - Mickey's Sorcerer Hat - Ability", 0x2DC6D1},
     {"Toy Story - Emperor Zurg's Wrath - Ability", 0x2DC6FE},
     {"The Sword in the Stone - Merlin's Summon - Ability", 0x2DC6FF},
     {"Mickey Mouse Universe - Mickey's Car - Toy (Vehicle)", 0x3D0912},
     {"Cinderella - Cinderella's Coach - Toy (Vehicle)", 0x3D0913},
     {"The Muppets - Electric Mayhem Bus - Toy (Vehicle)", 0x3D0914},
     {"101 Dalmatians - Cruella De Vil's Car - Toy (Vehicle)", 0x3D0915},
     {"Toy Story - Pizza Planet Delivery Truck - Toy (Vehicle)", 0x3D0916},
     {"Monsters, Inc. - Mike's New Car - Toy (Vehicle)", 0x3D0917},
     {"Disney Parks - Disney Parks Parking Lot Tram - Toy (Vehicle)", 0x3D0919},
     {"Peter Pan, Disney Parks - Jolly Roger - Toy (Aircraft)", 0x3D091A},
     {"Dumbo, Disney Parks - Dumbo the Flying Elephant - Toy (Aircraft)", 0x3D091B},
     {"Bolt - Calico Helicopter - Toy (Aircraft)", 0x3D091C},
     {"Tangled - Maximus - Toy (Mount)", 0x3D091D},
     {"Brave - Angus - Toy (Mount)", 0x3D091E},
     {"Aladdin - Abu the Elephant - Toy (Mount)", 0x3D091F},
     {"The Adventures of Ichabod and Mr. Toad - Headless Horseman's Horse - Toy (Mount)", 0x3D0920},
     {"Beauty and the Beast - Phillipe - Toy (Mount)", 0x3D0921},
     {"Mulan - Khan - Toy (Mount)", 0x3D0922},
     {"Tarzan - Tantor - Toy (Mount)", 0x3D0923},
     {"Mulan - Dragon Firework Cannon - Toy (Weapon)", 0x3D0924},
     {"Lilo & Stitch - Stitch's Blaster - Toy (Weapon)", 0x3D0925},
     {"Toy Story, Disney Parks - Toy Story Mania Blaster - Toy (Weapon)", 0x3D0926},
     {"Alice in Wonderland - Flamingo Croquet Mallet - Toy (Weapon)", 0x3D0927},
     {"Up - Carl Fredricksen's Cane - Toy (Weapon)", 0x3D0928},
     {"Lilo & Stitch - Hangin' Ten Stitch With Surfboard - Toy (Hoverboard)", 0x3D0929},
     {"Condorman - Condorman Glider - Toy (Glider)", 0x3D092A},
     {"WALL-E - WALL-E's Fire Extinguisher - Toy (Jetpack)", 0x3D092B},
     {"TRON - On the Grid - Customization (Terrain)", 0x3D092C},
     {"WALL-E - WALL-E's Collection - Customization (Terrain)", 0x3D092D},
     {"Wreck-It Ralph - King Candy's Dessert Toppings - Customization (Terrain)", 0x3D092E},
     {"Frankenweenie - Victor's Experiments - Customization (Terrain)", 0x3D0930},
     {"The Nightmare Before Christmas - Jack's Scary Decorations - Customization (Terrain)",
      0x3D0931},
     {"Frozen - Frozen Flourish - Customization (Terrain)", 0x3D0933},
     {"Tangled - Rapunzel's Kingdom - Customization (Terrain)", 0x3D0934},
     {"TRON - TRON Interface - Customization (Skydome)", 0x3D0935},
     {"WALL-E - Buy N Large Atmosphere - Customization (Skydome)", 0x3D0936},
     {"Wreck-It Ralph - Sugar Rush Sky - Customization (Skydome)", 0x3D0937},
     {"The Nightmare Before Christmas - Halloween Town Sky - Customization (Skydome)", 0x3D093A},
     {"Frozen - Chill in the Air - Customization (Skydome)", 0x3D093C},
     {"Tangled - Rapunzel's Birthday Sky - Customization (Skydome)", 0x3D093D},
     {"Toy Story, Disney Parks - Astro Blasters Space Cruiser - Toy (Vehicle)", 0x3D0940},
     {"Finding Nemo - Marlin's Reef - Customization (Terrain)", 0x3D0941},
     {"Finding Nemo - Nemo's Seascape - Customization (Skydome)", 0x3D0942},
     {"Alice in Wonderland - Alice's Wonderland - Customization (Terrain)", 0x3D0943},
     {"Alice in Wonderland - Tulgey Wood - Customization (Skydome)", 0x3D0944},
     {"Phineas and Ferb - Tri-State Area Terrain - Customization (Terrain)", 0x3D0945},
     {"Phineas and Ferb - Danville Sky - Customization (Skydome)", 0x3D0946}}};

static constexpr std::array<u8, 32> SHA1_CONSTANT = {
    0xAF, 0x62, 0xD2, 0xEC, 0x04, 0x91, 0x96, 0x8C, 0xC5, 0x2A, 0x1A, 0x71, 0x65, 0xF8, 0x65, 0xFE,
    0x28, 0x63, 0x29, 0x20, 0x44, 0x69, 0x73, 0x6e, 0x65, 0x79, 0x20, 0x32, 0x30, 0x31, 0x33};

InfinityUSB::InfinityUSB()
{
  infinity_queries_init(&m_queries, 16);
  infinity_waiting_init(&m_response_list, 16);
  m_vid = 0x0E6F;
  m_pid = 0x0129;
  m_id = (u64(m_vid) << 32 | u64(m_pid) << 16 | u64(9) << 8 | u64(1));
  m_device_descriptor = DeviceDescriptor{0x12,   0x1,    0x200, 0x0, 0x0, 0x0, 0x20,
                                         0x0E6F, 0x0129, 0x200, 0x1, 0x2, 0x3, 0x1};
  m_config_descriptor.emplace_back(ConfigDescriptor{0x9, 0x2, 0x29, 0x1, 0x1, 0x0, 0x80, 0xFA});
  m_interface_descriptor.emplace_back(
      InterfaceDescriptor{0x9, 0x4, 0x0, 0x0, 0x2, 0x3, 0x0, 0x0, 0x0});
  m_endpoint_descriptor.emplace_back(EndpointDescriptor{0x7, 0x5, 0x81, 0x3, 0x20, 0x1});
  m_endpoint_descriptor.emplace_back(EndpointDescriptor{0x7, 0x5, 0x1, 0x3, 0x20, 0x1});
}

InfinityUSB::~InfinityUSB()
{
  while (!infinity_waiting_empty(&m_response_list))
  {
    delete *infinity_waiting_front(&m_response_list);
    infinity_waiting_pop_front(&m_response_list);
  }
  infinity_waiting_free(&m_response_list);
  infinity_queries_free(&m_queries);
}

DeviceDescriptor InfinityUSB::GetDeviceDescriptor() const
{
  return m_device_descriptor;
}

std::vector<ConfigDescriptor> InfinityUSB::GetConfigurations() const
{
  return m_config_descriptor;
}

std::vector<InterfaceDescriptor> InfinityUSB::GetInterfaces(u8 config) const
{
  return m_interface_descriptor;
}

std::vector<EndpointDescriptor> InfinityUSB::GetEndpoints(u8 config, u8 iface, u8 alt) const
{
  return m_endpoint_descriptor;
}

bool InfinityUSB::Attach()
{
  if (m_device_attached)
  {
    return true;
  }
  DEBUG_LOG(IOS_USB, "[%04x:%04x] Opening device", (unsigned)m_vid, (unsigned)m_pid);
  m_device_attached = true;
  return true;
}

bool InfinityUSB::AttachAndChangeInterface(const u8 iface)
{
  if (!Attach())
    return false;

  if (iface != m_active_interface)
    return ChangeInterface(iface) == 0;

  return true;
}

int InfinityUSB::CancelTransfer(const u8 endpoint)
{
  INFO_LOG(IOS_USB, "[%04x:%04x %u] Cancelling transfers (endpoint %#x)", (unsigned)m_vid,
           (unsigned)m_pid, (unsigned)m_active_interface, (unsigned)endpoint);

  return IPC_SUCCESS;
}

int InfinityUSB::ChangeInterface(const u8 iface)
{
  DEBUG_LOG(IOS_USB, "[%04x:%04x %u] Changing interface to %u", (unsigned)m_vid, (unsigned)m_pid,
            (unsigned)m_active_interface, (unsigned)iface);
  m_active_interface = iface;
  return 0;
}

int InfinityUSB::GetNumberOfAltSettings(u8 iface)
{
  return 0;
}

int InfinityUSB::SetAltSetting(u8 alt_setting)
{
  return 0;
}

int InfinityUSB::SubmitTransfer(std::unique_ptr<CtrlMessage> cmd)
{
  DEBUG_LOG(IOS_USB,
            "[%04x:%04x %u] Control: bRequestType=%02x bRequest=%02x wValue=%04x"
            " wIndex=%04x wLength=%04x",
            (unsigned)m_vid, (unsigned)m_pid, (unsigned)m_active_interface,
            (unsigned)cmd->request_type, (unsigned)cmd->request, (unsigned)cmd->value,
            (unsigned)cmd->index, (unsigned)cmd->length);
  return 0;
}
int InfinityUSB::SubmitTransfer(std::unique_ptr<BulkMessage> cmd)
{
  DEBUG_LOG(IOS_USB, "[%04x:%04x %u] Bulk: length=%04x endpoint=%02x", (unsigned)m_vid,
            (unsigned)m_pid, (unsigned)m_active_interface, cmd->length, (unsigned)cmd->endpoint);
  return 0;
}
int InfinityUSB::SubmitTransfer(std::unique_ptr<IntrMessage> cmd)
{
  DEBUG_LOG(IOS_USB, "[%04x:%04x %u] Interrupt: length=%04x endpoint=%02x", (unsigned)m_vid,
            (unsigned)m_pid, (unsigned)m_active_interface, cmd->length, (unsigned)cmd->endpoint);

  auto& system = cmd->GetEmulationKernel().GetSystem();
  auto& memory = system.GetMemory();
  auto& infinity_base = system.GetInfinityBase();
  u8* buf = memory.GetPointerForRange(cmd->data_address, cmd->length);
  if (cmd->length != 32 || buf == nullptr)
  {
    ERROR_LOG(IOS_USB, "Infinity Base command invalid");
    return IPC_EINVAL;
  }

  std::array<u8, 32> data = {};
  std::array<u8, 32> response_data = {};
  // First call - constant device response
  if (buf[0] == 0x00)
  {
    IntrMessage* const waiting = cmd.release();
    infinity_waiting_push_back_growing(&m_response_list, &waiting);
  }
  // Respond with data requested from FF command
  else if (buf[0] == 0xAA || buf[0] == 0xAB)
  {
    // If a new figure has been added or removed, get the updated figure response
    if (infinity_base.HasFigureBeenAddedRemoved())
    {
      ScheduleTransfer(std::move(cmd), infinity_base.PopAddedRemovedResponse(), 1000);
    }
    else if (infinity_queries_empty(&m_queries))
    {
      IntrMessage* const waiting = cmd.release();
      infinity_waiting_push_back_growing(&m_response_list, &waiting);
    }
    else
    {
      const std::array<u8, 32> query = *infinity_queries_front(&m_queries);
      infinity_queries_pop_front(&m_queries);
      ScheduleTransfer(std::move(cmd), query, 1000);
    }
  }
  else if (buf[0] == 0xFF)
  {
    // FF <length of packet data> <packet data> <checksum>
    // <checksum> is the sum of all the bytes preceding it (including the FF and <packet length>).
    //
    // <packet data> consists of:
    // <command> <sequence> <optional attached data>
    //
    // <sequence> is an auto-incrementing sequence, so that the game can match up the command
    // packet with the response it goes with.
    // <length of packet data> includes <command>, <sequence> and <optional attached data>, but
    // not FF or <checksum>

    u8 command = buf[2];
    u8 sequence = buf[3];

    switch (command)
    {
    case 0x80:
    {
      // Activate Base, constant response (might be specific based on device type)
      response_data = {0xaa, 0x15, 0x00, 0x00, 0x0f, 0x01, 0x00, 0x03, 0x02, 0x09, 0x09, 0x43,
                       0x20, 0x32, 0x62, 0x36, 0x36, 0x4b, 0x34, 0x99, 0x67, 0x31, 0x93, 0x8c};
      break;
    }
    case 0x81:
    {
      // Initiate random number generation, combine the 8 big endian aligned bytes sent in the
      // command in to a u64, and descramble this number, which is then used to generate
      // the seed for the random number generator to align with the game. Finally, respond with a
      // blank packet as this command doesn't need a response.
      infinity_base.DescrambleAndSeed(buf, sequence, response_data);
      break;
    }
    case 0x83:
    {
      // Respond with random generated numbers based on the seed from the 0x81 command. The next
      // random number is 4 bytes, which then needs to be scrambled in to an 8 byte unsigned
      // integer, and then passed back as 8 big endian aligned bytes.
      infinity_base.GetNextAndScramble(sequence, response_data);
      break;
    }
    case 0x90:
    case 0x92:
    case 0x93:
    case 0x95:
    case 0x96:
    {
      // Set Colors. Needs further input to actually emulate the 'colors', but none of these
      // commands expect a response. Potential commands could include turning the lights on
      // individual sections of the base, flashing lights, setting colors, initiating a color
      // sequence etc.
      infinity_base.GetBlankResponse(sequence, response_data);
      break;
    }
    case 0xA1:
    {
      // A1 - Get Presence Status:
      // Returns what figures, if any, are present and the order they were added.
      // For each figure on the infinity base, 2 blocks are sent in the response:
      // <index> <unknown>
      // <index> is 20 for player 1, 30 for player 2, and 10 for the hexagonal position.
      // This is also added with the order the figure was added, so 22 would indicate player one,
      // which was added 3rd to the base.
      // <unknown> is (for me) always 09. If nothing is on the
      // infinity base, no data is sent in the response.
      infinity_base.GetPresentFigures(sequence, response_data);
      break;
    }
    case 0xA2:
    {
      // A2 - Read Figure Data:
      // This reads a block of 16 bytes from a figure on the infinity base.
      // Attached data: II BB UU
      // II is the order the figure was added (00 for first, 01 for second and 02 for third etc).
      // BB is the block number. 00 is block 1, 01 is block 4, 02 is block 8, and 03 is block 12).
      // UU is either 00 or 01 -- not exactly sure what it indicates.
      infinity_base.QueryBlock(buf[4], buf[5], response_data, sequence);
      break;
    }
    case 0xA3:
    {
      // A3 - Write Figure Data:
      // This writes a block of 16 bytes to a figure on the infinity base.
      // Attached data: II BB UU <16 bytes>
      // II is the order the figure was added (00 for first, 01 for second and 02 for third etc).
      // BB is the block number. 00 is block 1, 01 is block 4, 02 is block 8, and 03 is block 12).
      // UU is either 00 or 01 -- not exactly sure what it indicates.
      // <16 bytes> is the raw (encrypted) 16 bytes to be sent to the figure.
      infinity_base.WriteBlock(buf[4], buf[5], &buf[7], response_data, sequence);
      break;
    }
    case 0xB4:
    {
      // B4 - Get Tag ID:
      // This gets the tag's unique ID for a figure on the infinity base.
      // The first byte is the order the figure was added (00 for first, 01 for second and 02 for
      // third etc). The infinity base will respond with 7 bytes (the tag ID).
      infinity_base.GetFigureIdentifier(buf[4], sequence, response_data);
      break;
    }
    case 0xB5:
    {
      // Get status?
      infinity_base.GetBlankResponse(sequence, response_data);
      break;
    }
    default:
      ERROR_LOG(IOS_USB, "Unhandled Infinity Base Command: %u", (unsigned)command);
      break;
    }

    memcpy(data.data(), buf, 32);
    ScheduleTransfer(std::move(cmd), data, 500);
    if (infinity_waiting_empty(&m_response_list))
    {
      infinity_queries_push_back_growing(&m_queries, &response_data);
    }
    else
    {
      std::unique_ptr<IntrMessage> waiting(*infinity_waiting_front(&m_response_list));
      infinity_waiting_pop_front(&m_response_list);
      ScheduleTransfer(std::move(waiting), response_data, 1000);
    }
  }

  return 0;
}

int InfinityUSB::SubmitTransfer(std::unique_ptr<IsoMessage> cmd)
{
  DEBUG_LOG(IOS_USB, "[%04x:%04x %u] Isochronous: length=%04x endpoint=%02x num_packets=%02x",
            (unsigned)m_vid, (unsigned)m_pid, (unsigned)m_active_interface, cmd->length,
            (unsigned)cmd->endpoint, (unsigned)cmd->num_packets);
  return 0;
}

void InfinityUSB::ScheduleTransfer(std::unique_ptr<TransferCommand> command,
                                   const std::array<u8, 32>& data, u64 expected_time_us)
{
  command->FillBuffer(data.data(), 32);
  command->ScheduleTransferCompletion(32, expected_time_us);
}

InfinityBase::InfinityBase()
{
  infinity_queries_init(&m_figure_added_removed_response, 16);
}

InfinityBase::~InfinityBase()
{
  infinity_queries_free(&m_figure_added_removed_response);
}

bool InfinityBase::HasFigureBeenAddedRemoved() const
{
  return !infinity_queries_empty(&m_figure_added_removed_response);
}

std::array<u8, 32> InfinityBase::PopAddedRemovedResponse()
{
  std::array<u8, 32> response = *infinity_queries_front(&m_figure_added_removed_response);
  infinity_queries_pop_front(&m_figure_added_removed_response);
  return response;
}

u8 InfinityBase::GenerateChecksum(const std::array<u8, 32>& data, int num_of_bytes) const
{
  int checksum = 0;
  for (int i = 0; i < num_of_bytes; i++)
  {
    checksum += data[i];
  }
  return (checksum & 0xFF);
}

void InfinityBase::GetBlankResponse(u8 sequence, std::array<u8, 32>& reply_buf)
{
  reply_buf[0] = 0xaa;
  reply_buf[1] = 0x01;
  reply_buf[2] = sequence;
  reply_buf[3] = GenerateChecksum(reply_buf, 3);
}

void InfinityBase::GetPresentFigures(u8 sequence, std::array<u8, 32>& reply_buf)
{
  int x = 3;
  for (u8 i = 0; i < m_figures.size(); i++)
  {
    u8 slot = i == 0 ? 0x10 : (i > 0 && i < 4) ? 0x20 : 0x30;
    if (m_figures[i].present)
    {
      reply_buf[x] = slot + m_figures[i].order_added;
      reply_buf[x + 1] = 0x09;
      x = x + 2;
    }
  }
  reply_buf[0] = 0xaa;
  reply_buf[1] = x - 2;
  reply_buf[2] = sequence;
  reply_buf[x] = GenerateChecksum(reply_buf, x);
}

void InfinityBase::GetFigureIdentifier(u8 fig_num, u8 sequence, std::array<u8, 32>& reply_buf)
{

  InfinityFigure& figure = GetFigureByOrder(fig_num);

  reply_buf[0] = 0xaa;
  reply_buf[1] = 0x09;
  reply_buf[2] = sequence;
  reply_buf[3] = 0x00;

  if (figure.present)
  {
    memcpy(&reply_buf[4], figure.data.data(), 7);
  }
  reply_buf[11] = GenerateChecksum(reply_buf, 11);
}

void InfinityBase::QueryBlock(u8 fig_num, u8 block, std::array<u8, 32>& reply_buf, u8 sequence)
{

  InfinityFigure& figure = GetFigureByOrder(fig_num);

  reply_buf[0] = 0xaa;
  reply_buf[1] = 0x12;
  reply_buf[2] = sequence;
  reply_buf[3] = 0x00;
  u8 file_block = 0;
  if (block == 0)
  {
    file_block = 1;
  }
  else
  {
    file_block = block * 4;
  }
  if (figure.present && file_block < 20)
  {
    memcpy(&reply_buf[4], figure.data.data() + (16 * file_block), 16);
  }
  reply_buf[20] = GenerateChecksum(reply_buf, 20);
}

void InfinityBase::WriteBlock(u8 fig_num, u8 block, const u8* to_write_buf,
                              std::array<u8, 32>& reply_buf, u8 sequence)
{

  InfinityFigure& figure = GetFigureByOrder(fig_num);

  reply_buf[0] = 0xaa;
  reply_buf[1] = 0x02;
  reply_buf[2] = sequence;
  reply_buf[3] = 0x00;
  u8 file_block = 0;
  if (block == 0)
  {
    file_block = 1;
  }
  else
  {
    file_block = block * 4;
  }
  if (figure.present && file_block < 20)
  {
    memcpy(figure.data.data() + (file_block * 16), to_write_buf, 16);
    figure.Save();
  }
  reply_buf[4] = GenerateChecksum(reply_buf, 4);
}

std::string
InfinityBase::LoadFigure(const std::array<u8, INFINITY_NUM_BLOCKS * INFINITY_BLOCK_SIZE>& buf,
                         File::IOFile in_file, FigureUIPosition position)
{
  u8 order_added;

  std::vector<u8> sha1_calc = {SHA1_CONSTANT.begin(), SHA1_CONSTANT.end() - 1};
  for (int i = 0; i < 7; i++)
  {
    sha1_calc.push_back(buf[i]);
  }

  std::array<u8, 16> key = GenerateInfinityFigureKey(sha1_calc);

  struct aes_ctx ctx;
  std::array<u8, 16> infinity_decrypted_block = {};
  aes_init(&ctx, key.data(), key.size());
  aes_decrypt_block(&ctx, &buf[16], infinity_decrypted_block.data());

  u32 number = u32(infinity_decrypted_block[1]) << 16 | u32(infinity_decrypted_block[2]) << 8 |
               u32(infinity_decrypted_block[3]);
  DEBUG_LOG(IOS_USB, "Toy Number: %u", number);

  InfinityFigure& figure = m_figures[static_cast<u8>(position)];

  figure.inf_file = std::move(in_file);
  memcpy(figure.data.data(), buf.data(), figure.data.size());
  figure.present = true;
  if (figure.order_added == 255)
  {
    figure.order_added = m_figure_order;
    m_figure_order++;
  }
  order_added = figure.order_added;

  FigureBasePosition derived_position = DeriveFigurePosition(position);
  if (derived_position == FigureBasePosition::Unknown)
  {
    ERROR_LOG(IOS_USB, "Invalid Position for Infinity Figure");
    return "Unknown Figure";
  }

  std::array<u8, 32> figure_change_response = {0xab, 0x04,        static_cast<u8>(derived_position),
                                               0x09, order_added, 0x00};
  figure_change_response[6] = GenerateChecksum(figure_change_response, 6);
  infinity_queries_push_back_growing(&m_figure_added_removed_response, &figure_change_response);

  return FindFigure(number);
}

std::string InfinityBase::FindFigure(u32 number) const
{
  for (auto it = list_infinity_figures.begin(); it != list_infinity_figures.end(); it++)
  {
    if (it->second == number)
    {
      return it->first;
    }
  }
  return "Unknown Figure";
}

FigureBasePosition InfinityBase::DeriveFigurePosition(FigureUIPosition position)
{
  // In the added/removed response, position needs to be 1 for the hexagon, 2 for Player 1 and
  // Player 1's abilities, and 3 for Player 2 and Player 2's abilities. In the UI, positions 0, 1
  // and 2 represent the hexagon slot, 3, 4 and 5 represent Player 1's slot and 6, 7 and 8 represent
  // Player 2's slot.

  switch (position)
  {
  case FigureUIPosition::HexagonDiscOne:
  case FigureUIPosition::HexagonDiscTwo:
  case FigureUIPosition::HexagonDiscThree:
  {
    return FigureBasePosition::HexagonSlot;
  }
  case FigureUIPosition::PlayerOne:
  case FigureUIPosition::P1AbilityOne:
  case FigureUIPosition::P1AbilityTwo:
  {
    return FigureBasePosition::PlayerOneSlot;
  }
  case FigureUIPosition::PlayerTwo:
  case FigureUIPosition::P2AbilityOne:
  case FigureUIPosition::P2AbilityTwo:
  {
    return FigureBasePosition::PlayerTwoSlot;
  }
  default:
    return FigureBasePosition::Unknown;
  }
}

InfinityFigure& InfinityBase::GetFigureByOrder(u8 order_added)
{
  for (u8 i = 0; i < m_figures.size(); i++)
  {
    if (m_figures[i].order_added == order_added)
    {
      return m_figures[i];
    }
  }
  // This should never reach this statement, but return 0 reference to supress warnings
  ASSERT_MSG(IOS_USB, false, "Unable to find Disney Figure requested by game");
  return m_figures[0];
}

std::array<u8, 16> InfinityBase::GenerateInfinityFigureKey(const std::vector<u8>& sha1_data)
{
  Common::SHA1::Digest digest = Common::SHA1::CalculateDigest(sha1_data);
  // Infinity AES keys are the first 16 bytes of the SHA1 Digest, every set of 4 bytes need to be
  // reversed due to endianness
  std::array<u8, 16> key = {};
  for (int i = 0; i < 4; i++)
  {
    for (int x = 3; x >= 0; x--)
    {
      key[(3 - x) + (i * 4)] = digest[x + (i * 4)];
    }
  }
  return key;
}

void InfinityBase::DescrambleAndSeed(u8* buf, u8 sequence, std::array<u8, 32>& reply_buf)
{
  u64 value = u64(buf[4]) << 56 | u64(buf[5]) << 48 | u64(buf[6]) << 40 | u64(buf[7]) << 32 |
              u64(buf[8]) << 24 | u64(buf[9]) << 16 | u64(buf[10]) << 8 | u64(buf[11]);
  u32 seed = Descramble(value);
  GenerateSeed(seed);
  GetBlankResponse(sequence, reply_buf);
}

void InfinityBase::GetNextAndScramble(u8 sequence, std::array<u8, 32>& reply_buf)
{
  u32 next_random = GetNext();
  u64 scrambled_next_random = Scramble(next_random, 0);
  reply_buf = {0xAA, 0x09, sequence};
  reply_buf[3] = u8((scrambled_next_random >> 56) & 0xFF);
  reply_buf[4] = u8((scrambled_next_random >> 48) & 0xFF);
  reply_buf[5] = u8((scrambled_next_random >> 40) & 0xFF);
  reply_buf[6] = u8((scrambled_next_random >> 32) & 0xFF);
  reply_buf[7] = u8((scrambled_next_random >> 24) & 0xFF);
  reply_buf[8] = u8((scrambled_next_random >> 16) & 0xFF);
  reply_buf[9] = u8((scrambled_next_random >> 8) & 0xFF);
  reply_buf[10] = u8(scrambled_next_random & 0xFF);
  reply_buf[11] = GenerateChecksum(reply_buf, 11);
}

void InfinityBase::GenerateSeed(u32 seed)
{
  m_random_a = 0xF1EA5EED;
  m_random_b = seed;
  m_random_c = seed;
  m_random_d = seed;

  for (int i = 0; i < 23; i++)
  {
    GetNext();
  }
}

u32 InfinityBase::GetNext()
{
  u32 a = m_random_a;
  u32 b = m_random_b;
  u32 c = m_random_c;
  u32 ret = std::rotl(m_random_b, 27);

  u32 temp = (a + ((ret ^ 0xFFFFFFFF) + 1));
  b ^= std::rotl(c, 17);
  a = m_random_d;
  c += a;
  ret = b + temp;
  a += temp;

  m_random_c = a;
  m_random_a = b;
  m_random_b = c;
  m_random_d = ret;

  return ret;
}

u64 InfinityBase::Scramble(u32 num_to_scramble, u32 garbage)
{
  u64 mask = 0x8E55AA1B3999E8AA;
  u64 ret = 0;

  for (int i = 0; i < 64; i++)
  {
    ret <<= 1;

    if ((mask & 1) != 0)
    {
      ret |= (num_to_scramble & 1);
      num_to_scramble >>= 1;
    }
    else
    {
      ret |= (garbage & 1);
      garbage >>= 1;
    }

    mask >>= 1;
  }

  return ret;
}

u32 InfinityBase::Descramble(u64 num_to_descramble)
{
  u64 mask = 0x8E55AA1B3999E8AA;
  u32 ret = 0;

  for (int i = 0; i < 64; i++)
  {
    if (Common::ExtractBit(mask, 63))
    {
      ret = (ret << 1) | (num_to_descramble & 0x01);
    }

    num_to_descramble >>= 1;
    mask <<= 1;
  }

  return ret;
}

void InfinityFigure::Save()
{
  if (!inf_file)
    return;

  inf_file.Seek(0, File::SeekOrigin::Begin);
  inf_file.WriteBytes(data.data(), 0x40 * 0x05);
}
}  // namespace IOS::HLE::USB
