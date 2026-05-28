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
    - Find max_ch1 after reduction
    - Set ch1_threshold = 0.8 * max_ch1
3. If ch2 column contains data:
    - Find the min_ch2 of ch2 column and reduce every value in ch2 column by that min_ch2


## Detect the Rising Edges
Question: How to detect a 'rising edge' that crosses 80% in an array of data points?

To detect a "rising edge" that crosses an 80% threshold in an array, you can use NumPy to efficiently identify indices where the signal transitions from below the threshold to above it.

import numpy as np

def detect_rising_edges(data):
    # Convert to numpy array for performance
    arr = np.array(data)

    # Define the 80% threshold based on data range
    data_min, data_max = arr.min(), arr.max()
    threshold = data_min + 0.8 * (data_max - data_min)

    # Find where current point >= threshold AND previous point < threshold
    # The '1:' and ':-1' slices align current values with their predecessors
    rising_edges = np.where((arr[1:] >= threshold) & (arr[:-1] < threshold))[0] + 1

    return rising_edges, threshold

# Example Usage
data_points = [10, 12, 85, 90, 40, 20, 82, 95, 10]
indices, thresh = detect_rising_edges(data_points)

print(f"Calculated 80% Threshold: {thresh}")
print(f"Rising Edge Indices: {indices}")



## My Proposed Rising Edge Detector
1. Compress the data:
    - Find min_ch1
    - Find max_ch1
    - ch1_threshold = 0.8 * (max_ch1 - min_ch1)
    -



