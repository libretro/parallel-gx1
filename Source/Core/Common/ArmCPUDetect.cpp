// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/CPUDetect.h"
#include "Common/Hash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#ifdef __APPLE__
#include <sys/sysctl.h>
#elif defined(_WIN32)
#include <windows.h>
#include <arm64intr.h>
#include "Common/WindowsRegistry.h"
#elif defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#elif defined(HAVE_ELF_AUX_INFO)
#include <sys/auxv.h>
#elif defined(__OpenBSD__)
#include <machine/armreg.h>
#include <machine/cpu.h>
#endif

#ifdef __OpenBSD__
// clang-format off
#include <sys/types.h>
#include <sys/sysctl.h>
// clang-format on
#endif

#ifdef IPHONEOS
#include <regex>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#include <features/features_cpu.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

#if defined(__APPLE__) || defined(__FreeBSD__)

static bool SysctlByName(std::string* value, const std::string& name)
{
  size_t value_len = 0;
  if (sysctlbyname(name.c_str(), nullptr, &value_len, nullptr, 0))
    return false;

  value->resize(value_len);
  if (sysctlbyname(name.c_str(), value->data(), &value_len, nullptr, 0))
    return false;

  TruncateToCString(value);
  return true;
}

#endif

#if defined(_WIN32)

static constexpr char SUBKEY_CORE0[] = R"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)";

// Identifier: human-readable version of CPUID
// ProcessorNameString: marketing name of the processor
// VendorIdentifier: vendor company name
// There are some other maybe-interesting values nearby, BIOS info etc.
static bool ReadProcessorString(std::string* value, const std::string& name)
{
  return WindowsRegistry::ReadValue(value, SUBKEY_CORE0, name);
}

// Read cached register values from the registry
static bool ReadPrivilegedCPReg(u64* value, u32 reg)
{
  // Not sure if the value name is padded or not
  char name[16];
  snprintf(name, sizeof(name), "CP %x", reg);
  return WindowsRegistry::ReadValue(value, SUBKEY_CORE0, name);
}

static bool Read_MIDR_EL1(u64* value)
{
  return ReadPrivilegedCPReg(value, ARM64_SYSREG(0b11, 0, 0, 0b0000, 0));
}

static bool Read_ID_AA64ISAR0_EL1(u64* value)
{
  return ReadPrivilegedCPReg(value, ARM64_SYSREG(0b11, 0, 0, 0b0110, 0));
}

static bool Read_ID_AA64MMFR1_EL1(u64* value)
{
  return ReadPrivilegedCPReg(value, ARM64_SYSREG(0b11, 0, 0, 0b0111, 1));
}

#endif

#if defined(__linux__)

static bool ReadDeviceTree(std::string* value, const std::string& name)
{
  File::LineReader file(std::string("/proc/device-tree/") + name);
  const char* text;
  size_t length;
  size_t end = 0;
  if (!file.Next(&text, &length))
    return false;

  // the first word, as a stream extraction takes it
  while (end < length && text[end] && text[end] != ' ' && text[end] != '\t')
    end++;
  value->assign(text, end);
  return true;
}

static std::string ReadCpuinfoField(const std::string& field)
{
  File::LineReader file("/proc/cpuinfo");
  const char* text;
  size_t length;
  while (file.Next(&text, &length))
  {
    const std::string line(text, length);
    if (!line.starts_with(field))
      continue;
    auto non_tab = line.find_first_not_of("\t", field.length());
    if (non_tab == line.npos)
      continue;
    if (line[non_tab] != ':')
      continue;
    auto value_start = line.find_first_not_of(" ", non_tab + 1);
    if (value_start == line.npos)
      continue;
    return line.substr(value_start);
  }
  return {};
}

static bool Read_MIDR_EL1_Sysfs(u64* value)
{
  File::LineReader file("/sys/devices/system/cpu/cpu0/regs/identification/midr_el1");
  const char* text;
  size_t length;
  if (!file.Next(&text, &length))
    return false;

  *value = std::strtoull(std::string(text, length).c_str(), nullptr, 16);
  return true;
}

#endif

#if defined(__linux__) || defined(HAVE_ELF_AUX_INFO)

static u32 ReadHwCap(u32 type)
{
#if defined(__linux__)
  return getauxval(type);
#elif defined(HAVE_ELF_AUX_INFO)
  u_long hwcap = 0;
  elf_aux_info(type, &hwcap, sizeof(hwcap));
  return hwcap;
#endif
}

// For "Direct" reads, value gets filled via emulation, hence:
// "there is no guarantee that the value reflects the processor that it is currently executing on"
// On big.LITTLE systems, the value may be unrelated to the core this is invoked on, and unless
// other measures are taken, executing the instruction may cause the caller to be switched onto a
// different core when it resumes (and of course, caller could be preempted at any other time as
// well).
static inline u64 Read_MIDR_EL1_Direct()
{
  u64 value;
  __asm__ __volatile__("mrs %0, MIDR_EL1" : "=r"(value));
  return value;
}

static bool Read_MIDR_EL1(u64* value)
{
#ifdef __linux__
  if (Read_MIDR_EL1_Sysfs(value))
    return true;
#endif

  bool id_reg_user_access = ReadHwCap(AT_HWCAP) & HWCAP_CPUID;
#ifdef __FreeBSD__
  // FreeBSD kernel has support but doesn't seem to indicate it?
  // see user_mrs_handler
  id_reg_user_access = true;
#endif
  if (!id_reg_user_access)
    return false;
  *value = Read_MIDR_EL1_Direct();
  return true;
}

#endif

#if defined(_WIN32) || defined(__linux__) || defined(HAVE_ELF_AUX_INFO)

static std::string MIDRToString(u64 midr)
{
  u8 implementer = (midr >> 24) & 0xff;
  u8 variant = (midr >> 20) & 0xf;
  u8 arch = (midr >> 16) & 0xf;
  u16 part_num = (midr >> 4) & 0xfff;
  u8 revision = midr & 0xf;
  char buf[32];
  snprintf(buf, sizeof(buf), "%02X:%X:%u%u%u%u:%03X:%X", implementer, variant, (arch >> 3) & 1,
           (arch >> 2) & 1, (arch >> 1) & 1, arch & 1, part_num, revision);
  return buf;
}

#endif

CPUInfo cpu_info;

extern "C" int common_cpu_has_crc32(void)
{
  return cpu_info.bCRC32;
}

CPUInfo::CPUInfo()
{
  Detect();
}

void CPUInfo::Detect()
{
  vendor = CPUVendor::ARM;
  bFMA = true;
  bFlushToZero = true;

  num_cores = static_cast<int>(cpu_features_get_core_amount());
  if (num_cores < 1)
    num_cores = 1;

#ifdef __APPLE__
  SysctlByName(&model_name, "machdep.cpu.brand_string");

  // M-series CPUs have all of these
  // Apparently the world has accepted that these can be assumed supported "for all time".
  // see https://github.com/golang/go/issues/42747
  bAES = true;
  bSHA1 = true;
  bSHA2 = true;
  bCRC32 = true;

#ifdef IPHONEOS
  //
  // Check for CRC32 intrinsics support via the device's model. (Apple doesn't provide a better way
  // to do this.) A10 processors and above should support this.
  //
  // These include:
  //
  // iPhone9,1 and above
  // iPad7,1 and above
  // iPod9,1 and above
  // AppleTV6,2 and above
  //

  utsname system_info;
  uname(&system_info);

  const std::string model = std::string(system_info.machine);
  std::regex number_regex("[0-9]+");
  std::smatch match;

  if (std::regex_search(model.begin(), model.end(), match, number_regex))
  {
    int model_number = std::stoi(match[0]);
    int minimum_model = 99; // impossible value, in case a new type of device comes along

    if (model.find("iPhone") != std::string::npos)
    {
      minimum_model = 9;
    }
    else if (model.find("iPad") != std::string::npos)
    {
      minimum_model = 7;
    }
    else if (model.find("iPod") != std::string::npos)
    {
      minimum_model = 9;
    }
    else if (model.find("AppleTV") != std::string::npos)
    {
      minimum_model = 6;
    }

    bCRC32 = model_number >= minimum_model;
  }
#endif
#elif defined(_WIN32)
  // NOTE All this info is from cpu core 0 only.

  ReadProcessorString(&model_name, "ProcessorNameString");

  u64 reg = 0;
  // Attempt to be forward-compatible: perform inverted check against disabled feature states.
  if (Read_ID_AA64ISAR0_EL1(&reg))
  {
    bAES = ((reg >> 4) & 0xf) != 0;
    bSHA1 = ((reg >> 8) & 0xf) != 0;
    bSHA2 = ((reg >> 12) & 0xf) != 0;
    bCRC32 = ((reg >> 16) & 0xf) != 0;
  }
  if (Read_ID_AA64MMFR1_EL1(&reg))
  {
    // Introduced in Armv8.7, where AFP must be supported if AdvSIMD and FP both are.
    bAFP = ((reg >> 44) & 0xf) != 0;
  }
  // Pre-decoded MIDR_EL1 could be read with ReadProcessorString(.., "Identifier"),
  // but we want format to match across all platforms where possible.
  if (Read_MIDR_EL1(&reg))
  {
    cpu_id = MIDRToString(reg);
  }
#elif defined(__linux__) || defined(HAVE_ELF_AUX_INFO)
  // Linux, Android, FreeBSD and OpenBSD with elf_aux_info

#if defined(__FreeBSD__)
  SysctlByName(&model_name, "hw.model");
#elif defined(__OpenBSD__)
  int mib[2];
  size_t len;
  char hwmodel[256];

  mib[0] = CTL_HW;
  mib[1] = HW_MODEL;
  len = std::size(hwmodel);
  if (sysctl(mib, 2, &hwmodel, &len, nullptr, 0) != -1)
    model_name = std::string(hwmodel, len - 1);
#elif defined(__linux__)
  if (!ReadDeviceTree(&model_name, "model"))
  {
    // This doesn't seem to work on modern arm64 kernels
    model_name = ReadCpuinfoField("Hardware");
  }
#endif

  const u32 hwcap = ReadHwCap(AT_HWCAP);
  bAES = hwcap & HWCAP_AES;
  bCRC32 = hwcap & HWCAP_CRC32;
  bSHA1 = hwcap & HWCAP_SHA1;
  bSHA2 = hwcap & HWCAP_SHA2;

#if defined(AT_HWCAP2) && defined(HWCAP2_AFP)
  const u32 hwcap2 = ReadHwCap(AT_HWCAP2);
  bAFP = hwcap2 & HWCAP2_AFP;
#endif

  u64 midr = 0;
  if (Read_MIDR_EL1(&midr))
  {
    cpu_id = MIDRToString(midr);
  }
#elif defined(__OpenBSD__)
  // OpenBSD
  int mib[2];
  size_t len;
  char hwmodel[256];
  uint64_t isar0;

  mib[0] = CTL_HW;
  mib[1] = HW_MODEL;
  len = std::size(hwmodel);
  if (sysctl(mib, 2, &hwmodel, &len, nullptr, 0) != -1)
    model_name = std::string(hwmodel, len - 1);

  mib[0] = CTL_MACHDEP;
  mib[1] = CPU_ID_AA64ISAR0;
  len = sizeof(isar0);
  if (sysctl(mib, 2, &isar0, &len, nullptr, 0) != -1)
  {
    if (ID_AA64ISAR0_AES(isar0) >= ID_AA64ISAR0_AES_BASE)
      bAES = true;
    if (ID_AA64ISAR0_SHA1(isar0) >= ID_AA64ISAR0_SHA1_BASE)
      bSHA1 = true;
    if (ID_AA64ISAR0_SHA2(isar0) >= ID_AA64ISAR0_SHA2_BASE)
      bSHA2 = true;
    if (ID_AA64ISAR0_CRC32(isar0) >= ID_AA64ISAR0_CRC32_BASE)
      bCRC32 = true;
  }
#endif

  model_name = ReplaceAll(model_name, ",", "_");
  cpu_id = ReplaceAll(cpu_id, ",", "_");
}

std::string CPUInfo::Summarize()
{
  std::string sum = model_name;
  sum += ',';
  sum += cpu_id;

  if (bAFP)
    sum += ",AFP";
  if (bAES)
    sum += ",AES";
  if (bCRC32)
    sum += ",CRC32";
  if (bSHA1)
    sum += ",SHA1";
  if (bSHA2)
    sum += ",SHA2";

  return sum;
}
