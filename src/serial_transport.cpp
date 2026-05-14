#include "command_interface.h"

#include "binary_command_executor.h"
#include "serial_transport.h"

#include <ctype.h>
#include <stdlib.h>

namespace {

char inputBuffer[INPUT_BUFFER_SIZE];
size_t inputLength = 0U;
SerialTransportEncoding serialTransportEncoding = SerialTransportEncoding::Ascii;

bool equalsIgnoreCase(const char* lhs, const char* rhs)
{
    while (*lhs != '\0' && *rhs != '\0') {
        const char lc = static_cast<char>(tolower(static_cast<unsigned char>(*lhs)));
        const char rc = static_cast<char>(tolower(static_cast<unsigned char>(*rhs)));
        if (lc != rc) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return (*lhs == '\0' && *rhs == '\0');
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
    if (equalsIgnoreCase(encoding, "ascii")) {
        setSerialTransportEncoding(SerialTransportEncoding::Ascii);
        return true;
    } else if (equalsIgnoreCase(encoding, "binary")) {
        setSerialTransportEncoding(SerialTransportEncoding::Binary);
        return true;
    }
    return false;
}

bool SaTech::supportsEncoding(const char* encoding) const
{
    return encoding != nullptr &&
           (equalsIgnoreCase(encoding, "ascii") || equalsIgnoreCase(encoding, "binary"));
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
