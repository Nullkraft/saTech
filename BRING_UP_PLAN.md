# RF / Control-Board Bring-Up Plan

Use the already configured hardware MCP servers for closed-loop bring-up:
- `rigol_ds1102e` for the oscilloscope
- `bk390a` for the BK Precision 390A meter

Scope Auto policy:
- When Rigol Auto is needed, always use `rigol_ds1102e_protocol_command(key="auto_setup")`.
- Treat raw `:AUTO` writes as a low-level fallback only.

Do not widen the test surface until the current step passes.

## Step 0: Sanity Check

- [ ] Confirm the board is powered, serial is connected, and the technician binary transport is active.
- [ ] Confirm the Rigol is visible to the `rigol_ds1102e` MCP server.
- [ ] Confirm the BK390A port is visible to the `bk390a` MCP server.

Pass criteria:
- Serial input is accepted without framing errors.
- The scope server can identify a live device.
- The meter server can open a valid port.

Stop rule:
- If either instrument is not visible, fix the hardware connection before continuing.

## Step 1: Serial Transport

- [ ] Send the control word that switches the board into binary transport.
- [ ] Send one known command word and confirm the board accepts it.
- [ ] Send one known no-op or status word and confirm the board response matches the expected state.

Pass criteria:
- The board stays in binary mode after the mode switch.
- The command changes exactly one expected state.
- No unrelated state changes occur.

## Step 2: Attenuator Path

- [ ] Send one attenuator command word with a known code.
- [ ] Observe the attenuator CS pulse on the scope.
- [ ] Measure the attenuator output with the BK390A.
- [ ] Compare the measured result to the commanded step.

Pass criteria:
- The scope shows one clean CS transaction for the command.
- The meter reading matches the expected attenuator setting within the instrument tolerance.
- The board state reflects the commanded attenuator value.

Stop rule:
- Do not test any wider RF behavior until the attenuator path is correct.

## Step 3: LO2 Program Path

- [ ] Select LO2 explicitly.
- [ ] Send one FMN payload for LO2 programming.
- [ ] Observe the LO2 latch and SPI activity on the scope.
- [ ] Confirm the programming pulse shape and ordering.
- [ ] Confirm lock-detect or the equivalent board-visible indication after the write.

Pass criteria:
- The LO2 path writes only the intended device.
- The scope capture matches the expected write sequence.
- The board reports the expected LO2 state after the update.

## Step 4: ADC Return Path

- [ ] Reprogram LO2 once and return one ADC value for that event.
- [ ] Verify the count of returned ADC values matches the count of LO2 reprograms.
- [ ] Verify the returned ADC data decodes correctly at the host.
- [ ] If run-line encoding is enabled, verify the decoded stream matches the raw measurement sequence.

Pass criteria:
- One LO2 reprogram produces one ADC return entry.
- No extra ADC entries appear.
- The encoded or decoded host data matches the expected measurement sequence exactly.

## Step 5: LO1 / LO3 Spot Check

- [ ] Program LO1 once and confirm it still behaves as expected.
- [ ] Program LO3 once and confirm it still behaves as expected.
- [ ] Do not spend time here unless one of these paths fails.

Pass criteria:
- LO1 and LO3 still program correctly.
- The LO2 fast path remains the primary reference path.

## Step 6: Lock and Output Check

- [ ] Verify output select behavior for the active LO.
- [ ] Verify output power setting for the active LO.
- [ ] Verify lock detect after the final frequency update.

Pass criteria:
- Output select matches the command.
- Output power matches the command.
- Lock detect is stable after programming completes.

## Step 7: Record Keeping

- [ ] Record the command sent.
- [ ] Record the expected result.
- [ ] Record the scope observation.
- [ ] Record the meter reading.
- [ ] Record the next failure point if anything fails.

Pass criteria:
- Every step has a concrete observed result.
- Every failure stops the sequence at the first broken checkpoint.
