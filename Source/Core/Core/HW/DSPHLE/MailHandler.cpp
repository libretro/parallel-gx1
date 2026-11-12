// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/DSPHLE/MailHandler.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/DSP.h"

namespace DSP::HLE
{
CMailHandler::CMailHandler(DSP::DSPManager& dsp) : m_dsp(dsp)
{
  pending_mails_init(&m_pending_mails, 64);
}

CMailHandler::~CMailHandler()
{
  pending_mails_free(&m_pending_mails);
}

void CMailHandler::PushMail(u32 mail, bool interrupt, int cycles_into_future)
{
  const PendingMail pending = {mail, false};
  if (interrupt)
  {
    if (pending_mails_empty(&m_pending_mails))
    {
      m_dsp.GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP, cycles_into_future);
    }
    else
    {
      pending_mails_front(&m_pending_mails)->interrupt = true;
    }
  }
  pending_mails_push_back_growing(&m_pending_mails, &pending);
  DEBUG_LOG(DSP_MAIL, "DSP writes %#010x", mail);
}

u16 CMailHandler::ReadDSPMailboxHigh()
{
  // check if we have a mail for the CPU core
  if (!m_halted && !pending_mails_empty(&m_pending_mails))
  {
    m_last_mail = pending_mails_front(&m_pending_mails)->mail;
  }
  return u16(m_last_mail >> 0x10);
}

u16 CMailHandler::ReadDSPMailboxLow()
{
  // check if we have a mail for the CPU core
  if (!m_halted && !pending_mails_empty(&m_pending_mails))
  {
    const PendingMail* const front = pending_mails_front(&m_pending_mails);
    const bool generate_interrupt = front->interrupt;
    m_last_mail = front->mail;

    pending_mails_pop_front(&m_pending_mails);

    if (generate_interrupt)
    {
      m_dsp.GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);
    }
  }
  // Clear the top bit of the high mail word after the mail has been read.
  // The remaining bits read back the same as the previous mail, until new mail sent.
  // (The CPU reads the high word first, and then the low word; since this function returns the low
  // word, this means that the next read of the high word will have the top bit cleared.)
  m_last_mail &= ~0x8000'0000;
  return u16(m_last_mail & 0xffff);
}

void CMailHandler::ClearPending()
{
  pending_mails_clear(&m_pending_mails);
}

bool CMailHandler::HasPending() const
{
  return !pending_mails_empty(&m_pending_mails);
}

void CMailHandler::SetHalted(bool halt)
{
  m_halted = halt;
}

void CMailHandler::DoState(PointerWrap& p)
{
  // As a std::deque of pairs was: the count, then each mail and its interrupt flag.
  u32 size = static_cast<u32>(pending_mails_count(&m_pending_mails));
  u32 i;
  p.Do(size);
  if (p.IsReadMode())
  {
    pending_mails_clear(&m_pending_mails);
    for (i = 0; i < size; i++)
    {
      PendingMail pending = {0, false};
      p.Do(pending.mail);
      p.Do(pending.interrupt);
      pending_mails_push_back_growing(&m_pending_mails, &pending);
    }
  }
  else
  {
    for (i = 0; i < size; i++)
    {
      PendingMail* const pending = pending_mails_at(&m_pending_mails, i);
      p.Do(pending->mail);
      p.Do(pending->interrupt);
    }
  }
  p.Do(m_last_mail);
  p.Do(m_halted);
}
}  // namespace DSP::HLE
