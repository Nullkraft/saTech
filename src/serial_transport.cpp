#include "command_interface.h"

#include "binary_command_executor.h"
#include "serial_transport.h"

#include <ctype.h>
#include <stdlib.h>

namespace {

char inputBuffer[INPUT_BUFFER_SIZE];
size_t inputLength = 0U;
SerialTransportEncoding serialTransportEncoding = SerialTransportEncoding::Ascii;

void lowercaseCopy(char* destination, size_t destinationSize, const char* source)
{
    if (destinationSize == 0U) {
        return;
    }
    size_t i = 0U;
    for (; i + 1U < destinationSize && source[i] != '\0'; ++i) {
        destination[i] = static_cast<char>(tolower(static_cast<unsigned char>(source[i])));
    }
    destination[i] = '\0';
}

bool parseAsciiControlWord(const char* token, uint32_t* word)
{
    if (token == nullptr || word == nullptr) {
        return false;
    }
    char* endPointer = nullptr;
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
        uint32_t word = 0U;
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
    Serial.println(F("Input too long, line cleared."));
}

} // namespace

SaTech saTech;

bool SaTech::begin(const char* encoding)
{
    if (encoding == nullptr) {
        return false;
    }
    char normalized[7];
    lowercaseCopy(normalized, sizeof(normalized), encoding);
    if (strcmp(normalized, "ascii") == 0) {
        setSerialTransportEncoding(SerialTransportEncoding::Ascii);
        return true;
    } else if (strcmp(normalized, "binary") == 0) {
        setSerialTransportEncoding(SerialTransportEncoding::Binary);
        return true;
    }
    return false;
}

bool SaTech::supportsEncoding(const char* encoding) const
{
    if (encoding == nullptr) {
        return false;
    }
    char normalized[7];
    lowercaseCopy(normalized, sizeof(normalized), encoding);
    return strcmp(normalized, "ascii") == 0 || strcmp(normalized, "binary") == 0;
}

SerialTransportEncoding SaTech::transportEncoding() const
{
    return getSerialTransportEncoding();
}

void setSerialTransportEncoding(SerialTransportEncoding encoding)
{
    serialTransportEncoding = encoding;
}

SerialTransportEncoding getSerialTransportEncoding()
{
    return serialTransportEncoding;
}

void pollSerial()
{
    while (Serial.available() > 0) {
        const char incomingChar = static_cast<char>(Serial.read());
        const uint8_t incomingByte = static_cast<uint8_t>(incomingChar);
        if (serialTransportEncoding == SerialTransportEncoding::Binary) {
            processBinarySerialByte(incomingByte);
            continue;
        }
        collectAsciiByte(incomingChar, incomingByte);
    }
}
