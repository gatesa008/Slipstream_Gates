# SYSTEM service design (design only — not approved for implementation)

## Goal and non-goals

**Goal:** let an authenticated remote user see and operate:

- UAC consent and credential prompts,
- the Ctrl+Alt+Del (Winlogon) screen,
- the lock screen and logon screen, and
- the desktop of whichever session is on the console.

It must also survive logoff, user switching and reboot.

**Non-goals:** multiple simultaneous sessions; RDP-session streaming; bypassing any Windows security boundary. Everything below uses documented mechanisms available to SYSTEM.

## Why the