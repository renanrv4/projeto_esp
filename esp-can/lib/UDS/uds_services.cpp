#include <SPI.h>
#include "mcp_can.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "SPIFFS.h"
#include <vector>
#include "uds_services.h"

#define CHUNK_SIZE 2048
/**
 * @brief 
 * FUNCTIONAL requests are sent to all ECUs
 * PHYSICAL requests are sent to a specific ECU (BCM)
 */
const long FUNC_REQUEST_ID = 0x18DA40F1;
const long PHYS_REQUEST_ID = 0x18DA40F1;
const long PHYS_RESPONSE_ID = 0x18DAF140;

CANLogBin ramLog[RAM_LOG_SIZE];
volatile uint8_t ramLogIndex = 0;
volatile uint8_t ramLogCount = 0;

extern uint8_t ackBlock;

String logBuffer = "";
const int CS_PIN = 4;
MCP_CAN CAN(CS_PIN);
SemaphoreHandle_t canMutex;
SemaphoreHandle_t logMutex;

unsigned long startTime;

// ================== UTILITIES ==================
void addToRamLog(const CANLogBin& entry) {
    if (xSemaphoreTake(logMutex, portMAX_DELAY)) {
        ramLog[ramLogIndex] = entry;
        ramLogIndex = (ramLogIndex + 1) % RAM_LOG_SIZE;
        if (ramLogCount < RAM_LOG_SIZE)
            ramLogCount++;
        xSemaphoreGive(logMutex);
    }
}

void flushRamLogToSPIFFS() {
    if (!xSemaphoreTake(logMutex, portMAX_DELAY)) return;

    if (ramLogCount == 0) {
        xSemaphoreGive(logMutex);
        return;
    }

    File f = SPIFFS.open("/can.bin", FILE_APPEND);
    if (!f) {
        xSemaphoreGive(logMutex);
        return;
    }

    uint8_t start = (ramLogIndex + RAM_LOG_SIZE - ramLogCount) % RAM_LOG_SIZE;
    for (uint8_t i = 0; i < ramLogCount; i++) {
        uint8_t idx = (start + i) % RAM_LOG_SIZE;
        f.write((uint8_t*)&ramLog[idx], sizeof(CANLogBin));
    }
    f.close();
    ramLogCount = 0;
    xSemaphoreGive(logMutex);
}

String convertDecimalToHex(long int decimalNumber) {
    long int remainder,quotient;
    int i=1,j,temp;
    char hexadecimalNumber[100];
    String hexString = "";
    quotient = decimalNumber;

    while(quotient!=0){
        temp = quotient % 16;
        if( temp < 10)
            temp =temp + 48;
        else
            temp = temp + 55;

        hexadecimalNumber[i++]= temp;
        quotient = quotient / 16;
    }

    for(j = i -1; j > 0; j--) {
        hexString += String(hexadecimalNumber[j]);
    }
    return hexString;
}

String convertBytesToString(const byte* data, size_t length) {
    String result = "";
    for (size_t i = 0; i < length; i++) {
        if (data[i] < 0x10) {
            result += "0";
        }
        result += String(data[i], HEX);
        if (i < length - 1) {
            result += " ";
        }
    }
    result.toUpperCase();
    return result;
}

void sendMessage(const long id, byte* data, byte len, const char* description, String labelClass) {
    String requestString = convertBytesToString(data, len);
    if (CAN.sendMsgBuf(id, 1, len, data) == CAN_OK) {
        logBuffer += String("Sent: ") + description + "\n";
        CANLogBin entry;
        entry.timestamp = (millis() - startTime) / 1000.0;
        entry.canId = id;
        entry.dlc = len;
        memcpy(entry.data, data, len);
        entry.label = LABEL_NORMAL;
        addToRamLog(entry);
    } else {
        logBuffer += String("Error: Failed to send ") + description + "\n";
        CANLogBin entry;
        entry.timestamp = (millis() - startTime) / 1000.0;
        entry.canId = id;
        entry.dlc = 0;
        memset(entry.data, 0, 8);
        entry.label = LABEL_FAULT;
        addToRamLog(entry);
    }
}

void sendIdentificationCodeRequest() {
    /**
     * @brief 
     * ECU must reply with the following RDI:
     * $F187 : vehicleManufacturerSparePartNumber
     * $F190 : VIN
     * $F192 : systemSupplierECUHardwareNumber
     * $F193 : systemSupplierECUHardwareVersionNumber
     * $F194 : systemSupplierECUSoftwareNumber
     * $F195 : systemSupplierECUSoftwareVersionNumber
     * $F196 : exhaustRegulationOrTypeApprovalNumber (HomologationCode)
     * $F1A5 : ISO Code
     */
    byte udsRequestData[] = {0x03, 0x22, 0xF1, 0xA0};
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "IdentificationCode Request");
}

void sendReadDataByIdentifierVINCurrent() {
    byte udsRequestData[] = {0x03, 0x22, 0xF1, 0xB0}; // Read VIN Current
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "RDBI - VIN Current");
}

void sendReadDataByIdentifierVIN() {
    byte udsRequestData[] = {0x03, 0x22, 0xF1, 0x90}; // Read VIN
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "RDBI - VIN");
}

void sendFlowControl() {
    byte udsRequestData[] = {0x30, 0x00, 0x00};  // UDS FC (Flow Control) frame
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Send Flow Control");
}

void sendExtendedDiagnosticSessionRequest() {
    byte udsRequestData[] = {0x02, 0x10, 0x01};
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Extended Diagnostic Session Control");
}

void sendControlDTCSettingRequest() {
    byte udsRequestData[] = {0x02, 0x85, 0x02, 0x55, 0x55, 0x55, 0x55, 0x55}; 
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Control DTC Setting");
}

void sendCommunicationControlNoRxNoTxRequest() {
    byte udsRequestData[] = {0x03, 0x28, 0x03, 0x01, 0x55, 0x55, 0x55, 0x55};  // disableRxAndTx in the application
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Disable Tx and Rx");
}

void sendProgrammingDiagnosticSessionRequest() {
    byte udsRequestData[] = {0x02, 0x10, 0x02};
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Programming Session Control");
}

void sendSecurityRequestSeed() {
    byte udsRequestData[] = {0x02, 0x27, 0x01};
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Security Access Request Seed");
}

void sendSecurityKey(String key) {
    String byteString = "";

    // Remove "0x" or "0X" prefix if present
    if (key.startsWith("0x") || key.startsWith("0X")) {
        key = key.substring(2);
    }

    byte key_0 = strtoul((key.substring(0 * 2, 0 * 2 + 2)).c_str(), nullptr, 16);
    byte key_1 = strtoul((key.substring(1 * 2, 1 * 2 + 2)).c_str(), nullptr, 16);
    byte key_2 = strtoul((key.substring(2 * 2, 2 * 2 + 2)).c_str(), nullptr, 16);
    byte key_3 = strtoul((key.substring(3 * 2, 3 * 2 + 2)).c_str(), nullptr, 16);

    byte udsRequestData[] = {0x06, 0x27, 0x02, key_0, key_1, key_2, key_3};
        
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Sent Key");
}

void writeDataByIdAppSwFingerprint() {
    byte udsRequestData_FF[] = {0x10, 0x10, 0x2E, 0xF1, 0x84, 0x01, 0x24, 0x46};
    byte udsRequestData_CF01[] = {0x21, 0x32, 0x32, 0x38, 0x36, 0x33, 0x61, 0x20};
    byte udsRequestData_CF02[] = {0x22, 0x25, 0x08, 0x05};  
    sendMessage(PHYS_REQUEST_ID, udsRequestData_FF, sizeof(udsRequestData_FF), "Send WriteDataByIdentifierApplicationSwFingerprint");
    sendMessage(PHYS_REQUEST_ID, udsRequestData_CF01, sizeof(udsRequestData_CF01), "Send WriteDataByIdentifierApplicationSwFingerprint");
    sendMessage(PHYS_REQUEST_ID, udsRequestData_CF02, sizeof(udsRequestData_CF02), "Send WriteDataByIdentifierApplicationSwFingerprint");
}

void writeDataByIdAppDataFingerprint() {
    byte udsRequestData_FF[] = {0x10, 0x10, 0x2E, 0xF1, 0x85, 0x01, 0x24, 0x46};
    byte udsRequestData_CF01[] = {0x21, 0x32, 0x32, 0x38, 0x36, 0x33, 0x61, 0x20};
    byte udsRequestData_CF02[] = {0x22, 0x25, 0x08, 0x05};  
    sendMessage(PHYS_REQUEST_ID, udsRequestData_FF, sizeof(udsRequestData_FF), "Send WriteDataByIdentifierApplicationDataFingerprint");
    sendMessage(PHYS_REQUEST_ID, udsRequestData_CF01, sizeof(udsRequestData_CF01), "Send WriteDataByIdentifierApplicationDataFingerprint");
    sendMessage(PHYS_REQUEST_ID, udsRequestData_CF02, sizeof(udsRequestData_CF02), "Send WriteDataByIdentifierApplicationDataFingerprint");
}

void sendRoutineControleEraseMemory() {
    //RoutineControl (erase memory): 31 01
    byte udsRequestData_FF[] = {0x10, 0x0A, 0x31, 0x01, 0xFF, 0x00, 0x07, 0x40};
    byte udsRequestData_CF[] = {0x21, 0x00, 0x15, 0xFF, 0xFF};
    sendMessage(PHYS_REQUEST_ID, udsRequestData_FF, sizeof(udsRequestData_FF), "Send Flash Erase");  
    sendMessage(PHYS_REQUEST_ID, udsRequestData_CF, sizeof(udsRequestData_CF), "Send Flash Erase");  
}

void sendTesterPresent() {
    byte udsRequestData[] = {0x02, 0x3E, 0x80, 0x55, 0x55, 0x55, 0x55, 0x55};
    sendMessage(FUNC_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Tester Present");
}

void sendRoutineControlCheckRoutine() {
    //RoutineControl (erase memory): 31 03
    byte udsRequestData[] = {0x04, 0x31, 0x03, 0xFF, 0x00};
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Send Check Routine After Erase Memory");  
    
}

void sendDownloadRequest() {
    /**
     * @brief 
     * 34 00 33 07 40 00 0E C0 00
     * 34 : SID for Download
     * 00 : DataFormatIdentifier. First 0 means encryption method None. Second 0 means compression method None
     * 33 : AddressAndLengthFormatIdentifier. First 3 means memory address length of 3 bytes. Second 3 means memory size length of 3 bytes
     * 07 40 00 : start memory address
     * 0E C0 00 : memory size (966656 bytes)
     * Meaning: The client transfers 966,656 bytes to the server, starting at memory address 0x074000
     */
    byte udsRequestData_FF[] = {0x10, 0x09, 0x34, 0x00, 0x33, 0x07, 0x40, 0x00};
    byte udsRequestData_CF[] = {0x21, 0x0E, 0xC0, 0x00};
    sendMessage(PHYS_REQUEST_ID, udsRequestData_FF, sizeof(udsRequestData_FF), "Send Download Request");  
    //sendFlowControl();
    sendMessage(PHYS_REQUEST_ID, udsRequestData_CF, sizeof(udsRequestData_CF), "Send Download Request");  
}

void sendRequestTransferExit() {
    /**
     * @brief 
     * 37 : SID for Request Transfer Exit
     */
    byte udsRequestData[] = {0x01, 0x37};
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Send Request Transfer Exit");  
}


void sendRoutineControlChecksum() {
    //RoutineControl (erase memory): 31 01
    byte udsRequestData_FF[] = {0x10, 0x0C, 0x31, 0x01, 0xFF, 0x01, 0x07, 0x40};
    byte udsRequestData_CF[] = {0x21, 0x00, 0x15, 0xFF, 0xFF, 0xE9, 0x1E};
    sendMessage(PHYS_REQUEST_ID, udsRequestData_FF, sizeof(udsRequestData_FF), "Send Flash Erase");  
    //sendFlowControl();
    sendMessage(PHYS_REQUEST_ID, udsRequestData_CF, sizeof(udsRequestData_CF), "Send Flash Erase");  
}

void sendRoutineControlCheckRoutineDependencies() {
    //RoutineControl (check routine): 31 03
    byte udsRequestData[] = {0x04, 0x31, 0x03, 0xFF, 0x01};
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Send Check Routine Control Dependencies");  
    
}

void sendECUReset() {
    /**
     * @brief 
     * 11 01 : hard reset
     */
    byte udsRequestData[] = {0x02, 0x11, 0x01};
    sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Send ECU Reset - Hard Reset");  
}

String sendSeedToDllServer(String seed) {
  
  String url = "http://172.20.36.140:5000/calc_key?seed=" + seed + "&cnt=0&algoID=0x05";

  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();

  String key = "";

  if (httpCode > 0) {
    String payload = http.getString();

    // Allocate a JSON document
    StaticJsonDocument<200> doc;

    // Parse JSON
    DeserializationError error = deserializeJson(doc, payload);
    if (!error) {
      key = doc["key"].as<String>();
    } else {
      Serial.println("JSON parse error: " + String(error.c_str()));
    }
  } else {
    Serial.println("HTTP GET failed");
  }

  http.end();
  return key;

}

void sendKey(String key) {
  String byteString = "";

  // Remove "0x" or "0X" prefix if present
  if (key.startsWith("0x") || key.startsWith("0X")) {
    key = key.substring(2);
  }

  byte key_0 = strtoul((key.substring(0 * 2, 0 * 2 + 2)).c_str(), nullptr, 16);
  byte key_1 = strtoul((key.substring(1 * 2, 1 * 2 + 2)).c_str(), nullptr, 16);
  byte key_2 = strtoul((key.substring(2 * 2, 2 * 2 + 2)).c_str(), nullptr, 16);
  byte key_3 = strtoul((key.substring(3 * 2, 3 * 2 + 2)).c_str(), nullptr, 16);

  byte udsRequestData[] = {0x06, 0x27, 0x02, key_0, key_1, key_2, key_3};
    
  sendMessage(PHYS_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Sent Key");
}