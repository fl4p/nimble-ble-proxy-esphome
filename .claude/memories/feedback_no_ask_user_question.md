---
name: feedback-no-ask-user-question
description: "Don't use AskUserQuestion to resolve \"which port / which option\" decisions; just make the call or describe the choice in plain text."
metadata: 
  type: feedback
---

Don't use the `AskUserQuestion` tool for operational decisions like "which serial port should I flash" or "kill which Python process." The user interrupted exactly that prompt in Auto Mode and answered with new log output instead.

**Why:** In Auto Mode they want forward progress. Modal multi-choice prompts feel like over-asking, especially for reversible operational choices (killing a `idf.py monitor` is easy to re-attach).

**How to apply:** When blocked on a reversible operational choice, prefer one of: (a) make the call yourself and report what you did (b) describe the situation in plain text and let the user redirect inline (c) propose a default and run with it. Reserve `AskUserQuestion` for irreversible / design / scope decisions where mis-guessing has real cost.
