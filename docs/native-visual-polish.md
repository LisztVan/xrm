# Native Visual presentation

Visual uses the same XRM visual tokens as the shell and shared controls. It is a configuration browser with a structured preview, followed by the existing collection and multi-field editing dialogs. JSON remains the raw representation of the same draft.

## Information hierarchy

- The section navigator answers what can be configured. The selected section shows its human title, native path, and Draft / Read only state.
- Below 110 available content columns, navigation sits above the preview. At wider sizes a 31-column navigator sits beside it. Neither arrangement changes focus, Enter, Esc, or collection actions.
- DNS server previews make the address primary and port / query strategy secondary. A missing port says Default; a missing per-server strategy says Inherit. These are display labels, never newly written defaults.
- Routing previews preserve order and separate rule identification, target, and matching conditions. The collection uses Order, Rule / match, and Target columns; its form remains the existing native multi-field editor.
- Balancers, listeners, outbounds, hosts, levels, assets, and FakeDNS pools use domain columns rather than a generic Native object column. Environment values remain hidden from the overview.
- Object sections show configured native fields in aligned label/value rows. Missing source sections say Not configured; existing empty objects say No explicit settings. Unfamiliar legal representations remain explicitly native types with their JSON editing path.
- Form titles use readable domain names, and native dialogs use the shared button treatment. No additional persistence, defaults, metadata, or RPC calls were introduced.

## Self-critique and boundaries

The old section-only view left most of the module workspace empty and required opening a dialog before seeing configuration. The selected section now exposes useful data immediately. Wide terminals retain navigation and configuration side by side, while small terminals avoid allocating a permanent sidebar.

Collection previews are deliberately bounded; further items are counted and the full collection remains available on Enter. Long domain lists show the first entries and a remaining count; the native form and JSON retain the complete data. Configuration fields are not all promoted to equal visual weight, and unknown keys are preserved without turning the preview into a JSON dump.

This changes presentation only. It does not extend the field coverage in [module-form-coverage.md](module-form-coverage.md). Viewing remains non-mutating, every write uses the existing Read only gate, and child submissions still update only the parent draft.

## Verification

`c++ -std=c++23 -fsyntax-only -Isrc -Ibuild/dev/_deps/nlohmann_json-src/include -Ibuild/dev/_deps/ftxui_dep-src/include src/tui/forms/native_form.cpp` passed with the current native editor and shared token header.

After the integrated build, `build/component-rewrite/tests/tui/forms/xrm_tui_forms_tests --reporter compact` passed all 10 cases / 226 assertions. These cover all 15 module entry points, native field preservation, absent defaults, aliases and alternative types, DNS/routing object forms, collection ordering, and read-only boundaries.

Real terminal evidence is recorded with the visual-polish delivery; component tests alone are not a UI acceptance result.
