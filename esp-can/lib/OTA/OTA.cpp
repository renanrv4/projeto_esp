#include "cert.h"
#include "SPIFFS.h"
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <HTTPClient.h>
#include "OTA.h"


bool writtenToSPIFSS = false;

void flashESP32(String filename) {
    Serial.println("Starting ESP32 flash process...");

    File updateBin = SPIFFS.open(filename, FILE_READ);
    if (!updateBin) {
        Serial.println("Failed to open bin file for reading");
        return;
    }
    Serial.println(updateBin.size());
    if (!Update.begin(updateBin.size())) {
        
        Serial.println("Not enough space to begin OTA");
        updateBin.close();
        return;
    }

    size_t written = Update.writeStream(updateBin);
    if (written == updateBin.size()) {
        Serial.println("Written : " + String(written) + " successfully");
    } else {
        Serial.println("Written only : " + String(written) + "/" + String(updateBin.size()) + ". Retry?");
    }
    if (Update.end()) {
        Serial.println("OTA done!");
        if (Update.isFinished()) {
            Serial.println("Update successfully completed. Rebooting.");
            ESP.restart();
        } else {
            Serial.println("Update not finished. Something went wrong!");
        }
    } else {
        Serial.println("Error Occurred. Error #: " + String(Update.getError()));
    }
    updateBin.close();
}

bool validateSecurityKey(byte* receivedKey) {
    byte expectedKey[4] = {0x02, 0x03, 0x04, 0x05}; // Replace with the expected key based on the seed
    for (int i = 0; i < 4; i++) {
        if (receivedKey[i] != expectedKey[i]) {
            return false;
        }
    }
    return true;
}

bool downloadFirmware(const char* url, String filename) {
  
    Serial.println(url);
    WiFiClientSecure * client = new WiFiClientSecure;

    if (client) {
        client->setCACert(rootCACertificate);  // Set root CA certificate for HTTPS

        HTTPClient https;
        if (https.begin( * client, url)) {
            Serial.print("[HTTPS] GET...\n");
            delay(100);
            int httpCode = https.GET();
            if (httpCode == HTTP_CODE_MOVED_PERMANENTLY || httpCode == HTTP_CODE_FOUND || httpCode == HTTP_CODE_SEE_OTHER) {
              String redirectURL = https.getLocation();  // Get the new URL from the Location header
              Serial.print("Redirecting to: ");
              Serial.println(redirectURL);
              https.end();  // Close the initial connection
          
              // Begin a new connection with the redirect URL
              https.begin(* client, redirectURL);
              httpCode = https.GET();  // Make the new request
            }
            delay(100);
            if (httpCode == HTTP_CODE_OK) {
              
                Serial.println("Downloading file...");

                File file = SPIFFS.open(filename, FILE_WRITE);

                WiFiClient* stream = https.getStreamPtr();
                
                int totalDownloaded = 0;
                unsigned long startTime = millis();
                while (https.connected() && (https.getSize() > 0 || https.getSize() == -1)) {
                  static uint8_t buffer[16384];
                    size_t size = stream->available();
                    if (size) {
                        int bytesRead = stream->readBytes(buffer, ((size > sizeof(buffer)) ? sizeof(buffer) : size));
                        file.write(buffer, bytesRead);
                        totalDownloaded += bytesRead;
                        Serial.print("Downloaded: ");
                        Serial.print(totalDownloaded);
                        Serial.println(" bytes");
                    }
                     // Break the loop when all data has been downloaded
                    if (https.getSize() != -1 && totalDownloaded >= https.getSize()) {
                        break;
                    }
                    
                }

                file.close();
                Serial.print("Download completed in ");
                Serial.print(millis() - startTime);
                Serial.println(" ms");
                writtenToSPIFSS = true;
                delay(1000);
            } else {
                Serial.print("Error Occurred During Download: ");
                Serial.println(httpCode);
                
            }
            https.end();
        }
        delete client;
        return true;
    }
    return false;
}
