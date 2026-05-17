I need a script for sending SCPI commands to the oscilloscope.

1. When the script starts it immediately opens the port (usbtmc0) and keeps it open until I exit the script.
2. The script should run continuously until I type any one of the following:
    - Pressing Ctrl-C on the keyboard
    - Typing "exit" followed by 'enter' as a command
3. In general the SCPI commands I type should pass straight through to the scope
4. The SCPI commands should match the format found in:
    ~/projects/Arduino/saTech/docs/DS1000E(D)_ProgrammingGuide_EN.pdf
5. The SCPI is sent to the scope upon pressing 'enter'.
6. I should be able to press the 'up arrow' to scroll back through a history of commands:
    - History commands should be editable
    - The edited/selected command should be run by pressing 'enter'.
7. When I send the SCPI command ':WAVeform:DATA? [<source>]' where <source> is chan1 or chan2:
    - There should be a buffer for each channel should
    - Each data point needs to be one's complemented before storing to its buffer
    - Typing "save" followed by 'enter' will create/overwrite 'scope_dump.csv' with the buffers
    - The csv file will have 3 column headers: 'sample', 'ch1', and 'ch2'
    - If either ch1 or ch2, or both, are empty create/overwrite 'scope_dump.csv' with 'sample' data and just leave their columns empty if their data buffer is empty
    - Sample will be a sequential count starting from 0

## Data Analysis
I want to create a second script that can read and analyze the contents of scope_dump.csv

1. Open scope_dump.csv for read/write
2. If ch1 column contains data:
    - Find the min_ch1 of ch1 column and reduce every value in ch1 column by that min_ch1
3. If ch2 column contains data:
    - Find the min_ch2 of ch2 column and reduce every value in ch2 column by that min_ch2
