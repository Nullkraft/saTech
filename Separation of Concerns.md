Separate three concepts:

## Hardware description:
 - Durable board facts. Pin map, part inventory, board boundaries, signal names, active levels, maybe controller-board vs RF-board grouping.

 - Board_devices: C++ object ownership. The one real lo1, lo2, lo3, flash, later ADC/RAM objects.

 - Main_entry: Arduino lifecycle. Serial.begin(), setupBoardDevices(), tuneTo(), loop().

### The problem today is that main_entry.cpp is carrying all three.
This is why it feels wrong:

  - command_interface.h currently owns the pin map

  - command_interface.cpp owns CHIP_DEFINITIONS

  - main_entry.cpp owns device construction plus startup order.



### Start with one board_devices.h/.cpp; and grouping the objects with comments:

// RF board
~~~
extern ArduinoHAL halLo1;
extern ArduinoHAL halLo2;
extern ArduinoHAL halLo3;
extern MAX2871 lo1;
extern MAX2871 lo2;
extern MAX2871 lo3;
extern FrequencyCalculator freqCalc;

// Controller board
extern W25N_Flash flash;
~~~

Later, when ADC and SerialRAM actually land, if the file naturally turns into two blocks with separate setup routines, then split:

setupRfBoard();
setupControllerBoard();

The smallest useful design decision:

1. Move global hardware object definitions out of main_entry.cpp into board_devices.cpp.
2. Put their extern declarations in board_devices.h.
3. Leave pin constants and CHIP_DEFINITIONS alone for now unless you want command_interface renamed into something board-level later.

4. Keep main_entry.cpp as the entry point and call setup/init functions there.

Skipped: two-board file split now. Add it when ADC/RAM/Flash setup creates a second coherent startup block separate from LO/ref/atten setup.
