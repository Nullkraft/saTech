# RF / Control-Board Bring-Up Plan

- [ ] Verify the attenuator command word and confirm the board response with a single known test step.
- [ ] Set up the Rigol DS1102E and BK390A for narrow bring-up use: one scope channel, one meter mode, bounded reads.
- [ ] Confirm the attenuator hardware path end to end with the scope and meter before widening the test surface.
- [ ] Debug MAX2871 and `MAX2871_library_dev` behavior: output select, power, lock detect, and frequency programming.
- [ ] Troubleshoot sweep and control-board behavior only after the attenuator path is stable.
- [ ] Record each test result with command sent, expected outcome, measured result, and next failure point.
