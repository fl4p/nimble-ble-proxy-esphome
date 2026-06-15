---
name: feedback-alert-sound
description: "User wants an audible sound when Claude needs them to manually click something (BLE pairing button, browser prompts, etc.)"
metadata: 
  type: feedback
---

When you need the user to perform a manual UI action in this project — clicking the Web Bluetooth "Connect" button after a firmware reflash, granting an OS permission, picking a device from a chooser — play a short macOS system sound to alert them before or alongside the request.

**Why:** The user has the terminal in the background while a browser window is the active UI. They don't see the message asking them to click until they happen to check the chat. A sound cuts through.

**How to apply:** Run `afplay /System/Library/Sounds/Glass.aiff` (or `Hero.aiff` / `Funk.aiff` — Glass is short and pleasant) at the moment the prompt is needed. Background it (`afplay ... &`) so it doesn't block the conversation. Don't loop or repeat. One ping per prompt.

Trigger scenarios specific to this project:
- After `idf.py flash` completes and the user needs to click **Connect** in the Web Bluetooth dashboard.
- After reloading `web/index.html` for the same reason.
- Any other case where the next conversational step is blocked on a manual gesture in another window.
