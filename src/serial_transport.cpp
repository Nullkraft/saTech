#include "command_interface.h"

#include "binary_command_executor.h"

#include <ctype.h>
#include <stdlib.h>

namespace {

char inputBuffer[INPUT_BUFFER_SIZE];
size_t inputLength = 0U;
SerialEncoding serialEncoding = SerialEncoding::Ascii;

bool parseAsciiControlWord(const char* token, uint32_t* word)
{
    char* endPointer;
    *word = static_cast<uint32_t>(strtoul(token, &endPointer, 16));
    return endPointer != token && *endPointer == '\0';
}

void collectAsciiByte(char incoming, uint8_t incomingByte)
{
    if (incoming != '\r' && incoming != '\n' && isprint(incomingByte) == 0) {
        return;
    }
    if (incoming == '\r') {
        return;
    }
    if (incoming == '\n') {
        inputBuffer[inputLength] = '\0';
        uint32_t word;
        if (parseAsciiControlWord(inputBuffer, &word)) {
            processReceivedWord(word);
        }
        inputLength = 0U;
        return;
    }
    if (inputLength < (INPUT_BUFFER_SIZE - 1U)) {
        inputBuffer[inputLength++] = incoming;
        return;
    }
    inputLength = 0U;
}

} // namespace

SaTech saTech;

void SaTech::begin(SerialEncoding encoding)
{
    serialEncoding = encoding;
}

void setSerialEncoding(SerialEncoding encoding)
{
    serialEncoding = encoding;
}

void pollSerial()
{
    while (Serial.available() > 0) {
        const char incomingChar = static_cast<char>(Serial.read());
        const uint8_t incomingByte = static_cast<uint8_t>(incomingChar);
        if (serialEncoding == SerialEncoding::Binary) {
            processBinarySerialByte(incomingByte);
            continue;
        }
        collectAsciiByte(incomingChar, incomingByte);
    }
}
