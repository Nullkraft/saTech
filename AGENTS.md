## Decoder Operations
- Use 'expected register values' from max2871_expected.py to verify results decoded from Rigol mcp server decoder

## Coding routines
- When creating functions do not add error checking code. The technician be directly responsible for fixing any errors

## Testing routines
- For focused full-test Python work, use `./bin/python -m unittest discover -s test -p '*full_test.py'`
- Run the broader `./bin/python -m unittest discover -s test -p '*test.py'` before commits or when touching shared serial/command behavior

## Firmware size budget
- Treat 80% flash usage in `pio run -e technician` as a hard limit for new feature work
- When technician firmware reaches 80% flash, stop adding features and begin mitigation work first
