# XRM visual system: Slate

Presentation design only; `docs/xrm-interaction-spec.md` remains the sole interaction contract.

## Direction

| Candidate | Readability / density | State visibility | 80×24 | Cost |
|---|---|---|---|---|
| Minimal text tool | Easy reading, too little structure | Weak for View / Use / Edit / Run | Good | Low |
| Dense operator console | High throughput, too many competing regions | Strong but noisy | Poor | High |
| Developer console — selected | Strong headings, compact aligned data, restrained metadata | One focus language; explicit state words | Adaptive single column | Moderate |

Slate uses open sections and tables, with borders reserved for transient dialogs and notifications. The primary canvas belongs to business data. Chrome has a darker, quieter visual weight. Object detail stays in the bottom context region at every width; Overview separates actual runtime from saved configuration. A wide terminal adds relevant details rather than enlarging every gap.

## Tokens and component states

Source: `src/tui/components/style.hpp`. Default is **XRM Slate**, an explicit dark foreground/background. **XRM Paper** supplies a coordinated light palette; **Terminal palette** is an optional existing setting. No per-page color constants.

| Role | Slate | Meaning |
|---|---|---|
| default | `#dce3eb` | Business values and log messages |
| muted / disabled | `#92a3b7` | Labels, timestamps, metadata, read-only description |
| subtle | `#425368` | Separators |
| accent / focus | `#73c2e6` | Active navigation and focused controls |
| success / running | `#82c9a4` | Confirmed success or actual running state |
| warning | `#e4bb6f` | Dirty draft, warning, differing saved target |
| error | `#f28c91` | Failure and field validation |
| canvas / surface / selection | `#131a23` / `#1b2633` / `#233749` | Application, input/modal, focused selection |

Colors are reduced by FTXUI to the selected terminal capability (truecolor / 256 / 16). `NO_COLOR`, `TERM=dumb`, and No color suppress semantic colors; focus remains bold/reverse, selection has a cursor, booleans have `[x]`, radio choices have `(o)`, errors have `!`, dirty fields have `*`. UI semantics do not require Nerd Font or emoji.

Page titles and section titles are bold; primary values use normal high contrast; secondary values and metadata use muted ink. Underlining and flashing are avoided. Focused control/row uses one selection surface; an unfocused selected row retains its cursor and bold accent. Read-only and disabled modes are named, not inferred from dimming. Loading is textual, without decorative animation.

Spacing: one cell at screen edges; one cell between table columns; two cells between related detail values; one blank row between domain sections. Forms align labels in a 26-cell lane, use one blank row between fields, and keep field descriptions visible. Collection and multiline controls retain their dimensions as focus moves; finite choices use a compact one-line selector. Optional scalar values have a separate Set value control so omitted, zero and explicitly empty remain distinct. Multiline fields get a label above their input. Modal padding is one cell; buttons share `[ Action ]` styling. Footer is three rows plus separator: session identities, current object/mode, connection/focus/path and Help.

## Components

TextInput/TextArea share FTXUI editing with a surface background. Form labels indicate focus and local dirtiness; validation stays adjacent to the failing field. Select/Radio use `‹ choice ›`, Checkbox/MultiSelect use `[x]` / `[ ]`; these markers express different selection cardinality. CollectionEditor uses the same table, buttons and draft state. Dialog and Form share the modal boundary. Help presents the current task, selectable legal actions, prerequisites and local input/navigation instructions; aliases share one row. Toasts appear at the upper right, deferred while dialogs or writable JSON input have focus: the latest one on screens shorter than 30 rows, up to three on taller screens. Display text is capped at 120 / 180 glyphs respectively; full messages remain in notification history. Durations remain 3 / 5 / 8 seconds.

## Fonts

XRM controls its palette, spacing and emphasis. Font rendering is owned by the terminal emulator, so a portable TUI cannot bundle a font and force another terminal to use it. The optional [Kitty profile](terminal/kitty-xrm.conf) selects Maple Mono NF CN without changing the user's existing terminal configuration:

```sh
kitty --config docs/terminal/kitty-xrm.conf ./out/dev/xrm tui
```

Install the font from [Maple Mono's official releases](https://github.com/subframe7536/Maple-font/releases). [Kitty's font documentation](https://sw.kovidgoyal.net/kitty/kittens/choose-fonts/) explains terminal-side font selection. Emoji in user data depend on terminal/OS fallback fonts; no essential UI meaning uses emoji. The same UI works with an ordinary monospace font. XRM does not silently install fonts or alter desktop settings.

## Validation

Current-screen evidence, actual commands and page-by-page self-critique are recorded in `docs/design-review-remediation.md` (current) and `docs/evidence/visual-polish/README.md` (historical). Engineering validation does not sign off user G5 acceptance.

## State treatment

| State | Shared presentation |
|---|---|
| Normal | Default ink; quiet label, no frame |
| Focused | Selection surface and bold accent; field/row cursor |
| Selected | Retained cursor + bold, even after focus leaves |
| Active | Current navigation remains bold; running uses explicit text |
| Disabled | Muted with a named reason; omitted from executable Help actions |
| Read-only | READ ONLY heading; same readable data layout |
| Dirty | UNSAVED * heading and * next to changed field |
| Error | ! message next to the field; draft and focus retained |
| Loading | Existing pending-operation text and transient start receipt; no decorative animation |

## Review remediation rules (2026-09-27)

One Shell-owned modal frame, opaque theme-aware surfaces, whole-canvas backgrounds and visible empty inputs. Secondary navigation shows only whole labels with overflow directions and position. Bottom details stay accessible at 80 columns; no right Inspection or permanent action strip. Optional presence is separate from native values. Notification timers pause while a modal or writable JSON input hides them; no notification can cover the active editing field. The evidence and complete review disposition are in `design-review-remediation.md`.
