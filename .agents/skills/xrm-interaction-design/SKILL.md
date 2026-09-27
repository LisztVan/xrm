---
name: xrm-interaction-design
description: Apply XRM's terminal interaction contract when changing its TUI, CLI, help, status, or human-readable output.
---

# XRM interaction design

Read [the sole interaction contract](../../../docs/xrm-interaction-spec.md) before changing human interaction. Product/native boundaries remain in AGENTS.md, requirements.md and architecture.md. Historical audits and execution records are evidence, not active interaction rules.

Use is a nullable session toggle. Edit is the module editing mode of exactly one Profile; opening it sets View to that Profile but never Use/Run. Profile CRUD remains available without Edit. Gate every Modules write (Visual, JSON, include, preset, external editor and save) by the same matching Profile/Source condition. Resolve dirty drafts before changing edit context; failed saves keep mode, identity, text and focus.

Compose all pages from the shared FTXUI components. Never implement page-specific character editing or sequential field prompts. Forms show labeled controls together; finite values use selectors, collections have complete operations. Visual maps native semantics and preserves unknown fields and legal types. Source, draft and runtime publishing are distinct boundaries.

Keep object details/status at the bottom, actions and prerequisites in one executable contextual Help (F1 opens only, arrows select, Enter executes once), temporary feedback in expiring upper-right Toasts. F1 works inside forms; ? remains text in text inputs. Preserve TopNav/SecondaryNav/Content and local dialog focus. Existing tui-design advice cannot override this project contract (especially default navigation, footer shortcut bars or rewriting scope).

CLI help comes from the actual CLI11 command tree at each level; preserve machine schemas, pipes and exit contracts. Run focused Catch2/CLI checks and isolated PTY/current-screen checks using explicit temporary socket/database/HOME/XDG/confdir/tmux resources. Report actual evidence, supported input boundaries and missing coverage. Never mark user G5 acceptance PASS.
