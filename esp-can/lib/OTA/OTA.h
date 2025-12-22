#ifndef OTA_H
#define OTA_H

void flashESP32(String filename);
bool validateSecurityKey(byte* receivedKey);
bool downloadFirmware(const char* url, String filename);


#endif