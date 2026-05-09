# Instruction List

Extracted from `Instruction List 16-Oct-2023 6 rev-A.ods`.

## Attenuator (addr 0)

- [ ] Reserved for Command Flag `0000_00FF`
- [ ] Digital Atten (0-31.75dB) `007F_08FF`

## LO1 (addr 1)

- [ ] General Command `0000_01FF`
- [ ] RF OFF `0000_09FF`
- [ ] RF Set Power -4dBm `0000_11FF`
- [ ] RF Set Power -1dBm `0000_19FF`
- [ ] RF Set Power +2dBm `0000_21FF`
- [ ] RF Set Power +5dBm `0000_29FF`
- [ ] RF Set Frequency `0000_31FF`
- [ ] Mux set TriState `0000_39FF`
- [ ] Mux set Digital Lock Detect `0000_41FF`

## LO2 (addr 2)

- [ ] General Command `0000_02FF`
- [ ] RF OFF `0000_0AFF`
- [ ] RF Set Power -4dBm `0000_12FF`
- [ ] RF Set Power -1dBm `0000_1AFF`
- [ ] RF Set Power +2dBm `0000_22FF`
- [ ] RF Set Power +5dBm `0000_2AFF`
- [ ] RF Set Frequency `0000_32FF`
- [ ] Mux set TriState `0000_3AFF`
- [ ] Mux set Digital Lock Detect `0000_42FF`
- [ ] DIVA set Mode `0000_4AFF`

## LO3 (addr 3)

- [ ] General Command `0000_03FF`
- [ ] RF OFF `0000_0BFF`
- [ ] RF Set Power -4dBm `0000_13FF`
- [ ] RF Set Power -1dBm `0000_1BFF`
- [ ] RF Set Power +2dBm `0000_23FF`
- [ ] RF Set Power +5dBm `0000_2BFF`
- [ ] RF Set Frequency `0000_33FF`
- [ ] Mux set TriState `0000_3BFF`
- [ ] Mux set Digital Lock Detect `0000_43FF`
- [ ] DIVA set Mode `0000_4BFF`

## RefClocks (addr 4)

- [ ] All references off `0000_04FF`
- [ ] Reference 1 (66.000 MHz) `0000_0CFF`
- [ ] Reference 2 (66.666 MHz) `0000_14FF`

## CONTROL BOARD (addr 5)

- [ ] Select-adc1 `0000_05FF`
- [ ] Select-adc2 `0000_0DFF`
- [ ] Select-ram `0000_15FF`
- [ ] Select-flash `0000_1DFF`

## Comms State (addr 6)

- [ ] Enter-command `0000_06FF`
- [ ] Enter-fmn `0000_0EFF`
- [ ] Enter-direct `0000_16FF`

## Messages (addr 7)

- [ ] OFF - Arduino LED Off `0000_07FF`
- [ ] ON - Arduino LED On `0000_0FFF`
- [ ] Arduino Message Request `0000_17FF`
- [ ] Begin Sweep `0000_1FFF`
- [ ] End Sweep `0000_27FF`
- [ ] Reset Hardware & Report PLL Status `0000_2FFF`
- [ ] Begin Macro `0000_37FF`
- [ ] End Macro `0000_3FFF`
- [ ] Squelch Level `0000_47FF`
