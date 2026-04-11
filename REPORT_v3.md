# CR5L libdivecomputer Implementation Report

Scope:
- this report starts at the point where work moved into the forked `libdivecomputer` repository
- it does not repeat the earlier reverse-engineering history already captured in the analysis repository
- it records what was done in this repo, how it was done, and what was learned

## Starting Point

At the start of this phase:

- the reverse engineering had already produced a stable CR5L handoff bundle
- the handoff path was made available in this repo via:

```text
CR5L_HANDOFF_PATH.txt
```

- the handoff bundle included:
  - a detailed analysis report
  - a compact CR5L file-format spec
  - a `0x58` slot-mapping note
  - machine-readable 9-dive fixture exports
  - Python reference tools used during reverse engineering

The first implementation goal in this repo was:

- parser first
- BLE transport later

This was chosen because the parser can be validated deterministically against the exported fixture data, while the live BLE transport is more complex and more failure-prone.

## Step 1: Reconnect The Analysis To The New Repo

### What was done

We confirmed that the new fork contained:

- a one-file pointer to the handoff bundle:
  - `CR5L_HANDOFF_PATH.txt`

We then opened the handoff README and used it as the implementation entry point.

### How it was done

The repo was searched for a handoff/path marker and the absolute path from the pointer file was resolved.

The handoff bundle contents were inspected directly from the linked location rather than copied into the repo.

### What was learned

1. The pointer-file approach works cleanly for this workflow.
2. The implementation repo can stay minimally changed while still giving access to the analysis artifacts.
3. The handoff bundle is sufficient to start parser work without depending on the disposable earlier `libdivecomputer` checkout.

## Step 2: Identify The Integration Points In libdivecomputer

### What was done

We inspected the places that need changes for a new device family/parser:

- `include/libdivecomputer/common.h`
- `src/descriptor.c`
- `src/parser.c`
- `src/device.c`
- `src/Makefile.am`

We also checked existing parser/header patterns from similar devices.

### How it was done

Codebase searches and targeted file reads were used to find:

- family enum definitions
- descriptor registration
- parser registration
- device registration
- build inclusion

### What was learned

The minimal parser-first integration path is:

1. add a new family enum
2. add descriptor entry
3. add parser source/header
4. add temporary device-open stub
5. register everything in build/parser/device tables

This confirmed that we did not need to start with BLE transport code.

## Step 3: Add The CR5L Parser-First Scaffold

### What was done

We added a new CR5L family and parser scaffold.

Changed files:

- `include/libdivecomputer/common.h`
- `src/Makefile.am`
- `src/descriptor.c`
- `src/parser.c`
- `src/device.c`

New files:

- `src/crest_cr5l.h`
- `src/crest_cr5l.c`
- `src/crest_cr5l_parser.c`

### How it was done

The implementation followed the normal libdivecomputer pattern:

- a family enum was added:
  - `DC_FAMILY_CREST_CR5L`
- a descriptor entry was added for:
  - `Crest CR-5L`
- a BLE name filter was added for:
  - `CREST-CR5L`
- a parser module was added and registered
- a temporary device-open stub was added and registered

The device-open implementation currently returns:

```text
DC_STATUS_UNSUPPORTED
```

This is intentional because parser-first development was the priority and the BLE command transport is still a later task.

### What was learned

1. The CR5L can be introduced cleanly without entangling it with the existing Deep Six / CR-4 implementation.
2. A parser-first integration is feasible in libdivecomputer’s architecture.
3. A transport stub keeps the repo internally consistent while signaling that live-device support is not implemented yet.

## Step 4: Implement The Minimal CR5L Parser

### What was done

The first parser pass was implemented in:

- `src/crest_cr5l_parser.c`

The parser currently supports:

- payload validation as a likely CR5L record
- datetime from the confirmed UTC start timestamp
- summary fields:
  - divetime
  - max depth
  - avg depth
  - minimum temperature
- sample iteration from confirmed `0x51` records:
  - time
  - depth
  - temperature

The parser also skips over `0x58` blocks so they do not create false `0x51` hits.

### How it was done

The implementation was based directly on the already-confirmed findings from the handoff bundle:

- fixed header offsets
- `0x51 aa dd dd tt tt` sample structure
- checksum validation for `0x51`
- sequential walk through the detailed region
- best-effort skipping of valid `0x58` block structures

The sample timeline is currently derived from:

```text
duration / sample_count
```

which matches the reverse-engineering workflow and is good enough for a first parser pass.

### What was learned

1. The CR5L binary format is already solid enough to produce a practical parser skeleton in C.
2. The `0x51` records are strong enough to drive a useful first implementation without needing `0x58` to be exposed yet.
3. Skipping `0x58` blocks during the `0x51` scan is important to avoid false positives.

## Step 5: Complete Build Verification

### What was done

After the missing Homebrew autotools pieces were installed, we bootstrapped and built the repo using:

```text
autoreconf --install --force
```

```text
autoreconf --install --force
./configure
make -j1
```

The CR5L scaffold commit was then created after the full build succeeded.

### How it was done

The full autotools bootstrap and build were rerun on this branch after installing:

- `autoconf`
- `automake`
- `libtool`

The resulting build compiled and linked the new CR5L parser successfully through the normal libdivecomputer build flow.

### What was learned

1. The CR5L parser scaffold integrates cleanly with the existing autotools build.
2. The current blocking work is no longer “can it compile?” but “how do we validate parser behavior against the reverse-engineered fixtures?”
3. The local build produces extra generated documentation artifacts that should not be committed.

## Step 6: Make The Example CLI Understand CR5L

### What was done

We added the CR5L family alias to the example CLI backend table in:

- `examples/common.c`

The new family name is:

```text
cr5l
```

### How it was done

The `dctool` backend lookup table was extended so the existing `parse` command can be pointed at the new CR5L parser via:

```text
./examples/dctool -f cr5l parse ...
```

### What was learned

1. No separate one-off validation binary was needed.
2. The existing `dctool parse` path is already enough to exercise the CR5L parser through libdivecomputer’s public parser APIs.
3. This gives us a low-friction way to validate parser behavior against fixture dives before implementing BLE transport.

## Step 7: Add Fixture-Driven CR5L Parser Validation

### What was done

We added a fixture validation helper at:

- `tools/validate_cr5l_parser.py`

This validator:

- reads the handoff pointer file
- loads the exported CR5L fixture manifest
- locates the original reconstructed `.bin` payloads
- runs the built `examples/dctool` parser path for each dive
- compares the parsed output against the reverse-engineered expected values

Validated fields include:

- datetime
- divetime
- max depth
- avg depth
- minimum temperature
- sample count / representative sample points

### How it was done

Instead of building a new test subsystem, we reused the repo’s existing CLI example and validated the parser through the actual public parse path:

```text
./examples/dctool -f cr5l parse -o <tmp.xml> <raw.bin>
```

The validator parses the resulting XML and compares it to the exported fixture JSON from the handoff bundle.

### What was learned

1. The current CR5L parser validates cleanly across all 9 main reversed dive fixtures.
2. The parser is already strong enough to reproduce the key header fields and the `0x51` sample stream through libdivecomputer’s real parser interface.
3. One apparent discrepancy in `transfer_08` turned out to be a limitation in the older reverse-engineering exporter:
   - the exporter had an off-by-one end condition and dropped a valid final `0x51` sample at EOF
   - the library parser’s extra final sample is therefore plausible and not treated as a parser regression
4. The parser-validation problem is now solved well enough to move on to transport work.

## Current State

Implemented:

- CR5L family enum
- descriptor entry and BLE filter
- parser registration
- device registration
- parser-first CR5L scaffold
- minimal summary/sample decoding
- example CLI family alias for CR5L
- repeatable fixture-driven parser validation

Not implemented yet:

- live BLE transport/backend
- exposure of `0x58` tissue-state data through libdivecomputer fields
- transport-side fixture or protocol replay validation

## Immediate Next Steps

1. Start the CR5L BLE transport implementation in `src/crest_cr5l.c`.
2. Reconstruct the captured command flow inside libdivecomputer:
   - enumerate dives
   - select/open a dive
   - request payload
   - finalize/close
3. Reuse the already-validated parser for downloaded payload decoding.
4. Decide whether `0x58` stays internal for v1 or should be surfaced later via new parser fields.
