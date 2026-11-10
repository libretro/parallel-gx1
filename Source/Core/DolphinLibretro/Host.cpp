
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#ifdef __ANDROID__
#include "InputCommon/GCAdapter.h"
#include "InputCommon/GCPadStatus.h"
// TODO: removed
//#include "InputCommon/ControllerInterface/Touch/ButtonManager.h"
#endif

#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"
#include "Core/Host.h"

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

bool Host_TASInputHasFocus() { return false; }

void Host_Message(HostMessageID id)
{
  DEBUG_LOG_FMT(COMMON, "message id: {}\n", (int)id);
}

void Host_UpdateTitle(const std::string& title)
{
#if 0
  DEBUG_LOG_FMT(COMMON, "title : {}\n", title.c_str());
#endif
}

void Host_RequestRenderWindowSize(int width, int height)
{
}

bool Host_RendererHasFocus()
{
  /* called on input poll */
  return true;
}

bool Host_RendererHasFullFocus()
{
  return true;
}

void Host_TitleChanged()
{
}

bool Host_UIBlocksControllerState()
{
  return false;
}

#ifdef __ANDROID__

namespace GCAdapter
{
void SetAdapterCallback(std::function<void(void)> func)
{
}

void Init()
{
}

void StartScanThread()
{
}

void StopScanThread()
{
}

void Shutdown()
{
}

GCPadStatus Input(int chan)
{
  return {};
}

bool DeviceConnected(int chan)
{
  return false;
}

bool UseAdapter()
{
  return false;
}

void ResetRumble()
{
}

void Output(int chan, u8 rumble_command)
{
}

bool IsDetected(const char** error_message)
{
  return false;
}

bool IsDriverDetected()
{
  return false;
}

void ResetDeviceType(int chan)
{
}
}  // namespace GCAdapter

// TODO: refactor or remove?
/*namespace ButtonManager
{
bool GetButtonPressed(int padID, ButtonType button)
{
  return false;
}
float GetAxisValue(int padID, ButtonType axis)
{
  return 0;
}
}  // namespace ButtonManager
*/
#endif
