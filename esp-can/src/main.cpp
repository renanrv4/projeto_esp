#include <SPI.h>
#include "mcp_can.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "SPIFFS.h"
#include <ArduinoJson.h>
#include "OTA.h"
#include "uds_services.h"

const char* ssid = "RENAN97";
const char* password = "renan123";

WebServer server(80);

const int CAN_INT_PIN = 27;
const int CAN_DEBUG_LED = 25;

bool startRequests = false;
bool startOTAUpdate = false;
String urlUpdate = "";
String output = "";
String seed = "";
String key = "";
bool unlocked = false;
bool recEDS = false;
bool recProg = false;
bool recSeed = false;
bool swFingerprint = false;
bool appFingerprint = false;
bool erased = false;
bool routineChecked = false;
bool downloadRequest = false;
bool checksumOk = false;
bool finalDep = false;
bool exited = false;
bool reset = false;
bool alreadyDownloaded = false;

volatile uint8_t ackBlock = 0;

volatile bool flashInProgress = false;
bool ecuBusyProcessing = false;
bool blockAckReceived = false;

static uint16_t blockCounter = 1;
File transferFile;
uint8_t buffer[2048];

unsigned long lastFrameTime = 0;
volatile bool allDataTransferred = false;
const char* firmwarePath = "/firmware.bin"; // Path in SPIFFS to store the firmware file

int LED2 = 2;

// Unlock states
typedef enum {
    UNLOCK_IDLE,
    UNLOCK_EXT_DIAG_SESSION,
    UNLOCK_PROG_SESSION,
    UNLOCK_REQ_SEED,
    UNLOCK_SEND_KEY,
    UNLOCK_DONE
} UnlockState;

typedef enum FlashState {
    FS_WRITE_SW_FP = 0,
    FS_WRITE_APP_FP,
    FS_ERASE_MEMORY,
    FS_CHECK_ERASE,
    FS_DOWNLOAD_REQUEST,
    FS_TRANSFER_DATA,
    FS_TRANSFER_SEND,
    FS_TRANSFER_WAIT_ACK,
    FS_TRANSFER_EXIT,
    FS_CHECKSUM,
    FS_CHECK_DEPS,
    FS_ECU_RESET,
    FS_DONE
} FlashState;

unsigned long lastFlush = 0;

// ================== CAN Receive ==================
void receiveCANMessage(String labelClass="NORMAL") {
    long unsigned int rxId;
    unsigned char len = 0;
    unsigned char rxBuf[8];

    if (CAN.checkReceive() == CAN_MSGAVAIL) {
        CAN.readMsgBuf(&rxId, &len, rxBuf);
        if (rxId == 0x98DAF140) { // BCM ECU Response ID
            float relativeTime = (millis() - startTime) / 1000.0;
            String dataHex = "";
            String dataComp = "";
            Serial.print("Received: ");
            logBuffer += "Received: ";
            for (int i = 0; i < len; i++) {
                if(rxBuf[i] < 0x10) { dataHex += "0"; }
                dataHex += String(rxBuf[i], HEX);
                dataComp += String(rxBuf[i], HEX) + " ";
                if(i < len - 1) { dataHex += " "; }
                Serial.print(rxBuf[i], HEX);
                Serial.print(" ");
                logBuffer += String(rxBuf[i], HEX) + " ";
            }
            Serial.println();
            output = logBuffer;
            
            //===================================================================
            // Se output.indexOf não funcionar usar dataComp == "String"       //
            //===================================================================

            // === State transitions ===
            if(output.indexOf("6 50 1 0 32 1 f4") > 0) recEDS = true;
            if(output.indexOf("6 50 2 0 32 1 f4") > 0) recProg = true;
            if(output.indexOf("6 67 1") > 0) {
                seed = "0x" + String(rxBuf[3], HEX) + String(rxBuf[4], HEX) + String(rxBuf[5], HEX) + String(rxBuf[6], HEX);
                //Serial.println("Seed: " + seed);
                recSeed = true;
            }
            if(output.indexOf("2 67 2") >= 0) {
                //Serial.println("BCM ECU unlocked!");
                unlocked = true;
            }
            if(output.indexOf("3 6e f1 84") > 0) swFingerprint = true;
            if(output.indexOf("3 6e f1 85") > 0) appFingerprint = true;
            if(output.indexOf("4 71 1 ff 0") > 0) erased = true;
            if(output.indexOf("4 71 3 ff 0") > 0) routineChecked = true;
            if(output.indexOf("4 74 20 8 2") > 0) downloadRequest = true;
            
            // --- TransferData Positive Response (ACK per block) ---//
            if (len >= 3 && rxBuf[1] == 0x76) {
                ackBlock = rxBuf[2];
                Serial.print("✅ ECU ACK block: "); 
                Serial.println(ackBlock, HEX);
                blockAckReceived = true;
            }
            
            // --- Log CAN message ---
            CANLogBin entry;
            entry.timestamp = relativeTime;
            entry.canId = rxId;
            entry.dlc = len;
            memcpy(entry.data, rxBuf, len);
            entry.label = LABEL_NORMAL;

            if (xQueueSend(logQueue, &entry, 0) != pdTRUE) {
                droppedLogs++;
            }

            if(rxBuf[0] == 0x01 && rxBuf[1] == 0x77) exited = true;

            if(rxBuf[0] == 0x04 && rxBuf[1] == 0x71 && rxBuf[2] == 0x01
            && rxBuf[3] == 0xFF && rxBuf[4] == 0x01) checksumOk = true;
            
            if(rxBuf[0] == 0x04 && rxBuf[1] == 0x71 && rxBuf[2] == 0x03
            && rxBuf[3] == 0xFF && rxBuf[4] == 0x01) finalDep = true;

            if(rxBuf[0] == 0x02 && rxBuf[1] == 0x51 && rxBuf[2] == 0x01) reset = true;
            
        }
    }
}

// --- Send a single block (0x36 TransferData) ---
void sendUDSBlock(uint8_t* data, size_t len, uint8_t blockCounter) { 
    uint8_t frame[8]; 
    size_t offset = 0; 
    uint16_t totalLen = len + 2; // SID(0x36) + block counter + payload
    // ---- First Frame ---- 
    frame[0] = 0x10 | ((totalLen >> 8) & 0x0F); // ISO-TP First Frame header 
    frame[1] = totalLen & 0xFF; // lower 8 bits of total length 
    frame[2] = 0x36; // TransferData SID 
    frame[3] = blockCounter; // Block counter
    size_t firstData = std::min((size_t)4, len); // 4 bytes left in first CAN frame 
    memcpy(&frame[4], data, firstData); 
    memset(&frame[4 + firstData], 0x00, 4 - firstData); 
    sendMessage(0x18DA40F1, frame, 8, "UDS First Frame"); 
    //Serial.printf("Block %u - Sent First Frame: ", blockCounter); 
    for (int i = 0; i < 8; i++) Serial.printf("%02X ", frame[i]); 
    Serial.println(); 
    offset += firstData; 
    //sendFlowControl(); 
    // ---- Consecutive Frames ---- 

    uint8_t seq = 1; 
    while (offset < len) { 
        frame[0] = 0x20 | (seq & 0x0F); // CF PCI: 0x21, 0x22, 0x23 ... 
        size_t chunk = std::min((size_t)7, len - offset); 
        memcpy(&frame[1], &data[offset], chunk); 
        memset(&frame[1 + chunk], 0x00, 7 - chunk); 
        sendMessage(0x18DA40F1, frame, 8, "UDS Consecutive Frame"); 
        offset += chunk; 
        seq++; 
    } 
    Serial.printf("✅ Finished sending Block %u (%u bytes)\n", blockCounter, len); 
}

bool sendTransferBlock(const char* path)
{
    // Open file if first block
    if (blockCounter == 1) {
        transferFile = SPIFFS.open(path, "r");
        if (!transferFile) {
            Serial.println("❌ Cannot open firmware file");
            return false;
        }
        flashInProgress = true;
    }

    if (!transferFile.available()) {
        return false;
    }


    // Load next block
    memset(buffer, 0xFF, sizeof(buffer));
    size_t bytesRead = transferFile.read(buffer, sizeof(buffer));

    Serial.printf("📦 Sending block %u (%u bytes)\n", blockCounter, bytesRead);

    blockAckReceived = false;

    // Send UDS block
    if (xSemaphoreTake(canMutex, pdMS_TO_TICKS(50))) {
        sendUDSBlock(buffer, bytesRead, blockCounter);
        xSemaphoreGive(canMutex);
    }

    return true;  // block was sent
}


// ================== CAN Polling Task ==================
void canTask(void *pvParameters) {
    for (;;) {
        if (xSemaphoreTake(canMutex, 10 / portTICK_PERIOD_MS)) {
            receiveCANMessage();
            xSemaphoreGive(canMutex);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

bool udsIsBlockAcked() {
    return true;
}

// ================== Unlock Task ==================
void unlockTask(void *pvParameters) {

    UnlockState state = UNLOCK_IDLE;
    bool keySent = false;

    for (;;) {

        if (!startRequests || unlocked) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        switch (state) {

            // -----------------------------------------------------------
            case UNLOCK_IDLE:
                Serial.println("[STATE] UNLOCK_IDLE");
                state = UNLOCK_EXT_DIAG_SESSION;
                break;

            // -----------------------------------------------------------
            case UNLOCK_EXT_DIAG_SESSION:
                if (!recEDS) {
                    if (xSemaphoreTake(canMutex, portMAX_DELAY)) {
                        sendExtendedDiagnosticSessionRequest();
                        xSemaphoreGive(canMutex);
                    }
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                } else {
                    Serial.println("[STATE] ExtendedDiagnosticSession OK");
                    state = UNLOCK_PROG_SESSION;
                }
                break;

            // -----------------------------------------------------------
            case UNLOCK_PROG_SESSION:
                if (!recProg) {
                    if (xSemaphoreTake(canMutex, portMAX_DELAY)) {
                        sendProgrammingDiagnosticSessionRequest();
                        xSemaphoreGive(canMutex);
                    }
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                } else {
                    Serial.println("[STATE] ProgrammingSession OK");
                    state = UNLOCK_REQ_SEED;
                }
                break;

            // -----------------------------------------------------------
            case UNLOCK_REQ_SEED:
                if (!recSeed) {
                    if (xSemaphoreTake(canMutex, portMAX_DELAY)) {
                        sendSecurityRequestSeed();
                        xSemaphoreGive(canMutex);
                    }
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                } else {
                    Serial.println("[STATE] Seed received");
                    state = UNLOCK_SEND_KEY;
                }
                break;

            // -----------------------------------------------------------
            case UNLOCK_SEND_KEY:
                if (!keySent) {

                    key = sendSeedToDllServer(seed);
                    Serial.printf("Calculated key: %u\n", key);

                    if (xSemaphoreTake(canMutex, portMAX_DELAY)) {
                        sendKey(key);
                        xSemaphoreGive(canMutex);
                    }

                    keySent = true;
                    vTaskDelay(1000 / portTICK_PERIOD_MS); // wait for ECU reply
                }

                if (unlocked) {
                    Serial.println("ECU successfully unlocked!");
                    state = UNLOCK_DONE;
                } else {
                    Serial.println("ECU unlock failed — retrying...");
                    keySent = false;
                    recSeed = false; // force a new seed request
                    state = UNLOCK_REQ_SEED;
                }
                break;

            // -----------------------------------------------------------
            case UNLOCK_DONE:
                Serial.println("[STATE] UNLOCK DONE — suspending task.");
                vTaskSuspend(NULL);  // Unlock process ends
                break;
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ================== TP Task ==================
void testerPresentTask(void *param) {
    const unsigned long intervalMs = 3000; // 2 seconds
    uint8_t udsTP[] = {0x02, 0x3E, 0x80}; // Tester Present

    while (true) {
        if (startRequests && ecuBusyProcessing) {
            if (xSemaphoreTake(canMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                sendTesterPresent();
                xSemaphoreGive(canMutex);
                Serial.println("🟢 Sent Tester Present");
            }
        }
        vTaskDelay(intervalMs / portTICK_PERIOD_MS);
    }
}


// ================== Flash Task ==================


void flashTask(void *pvParameters) {

    FlashState flashState = FS_WRITE_SW_FP;
    for (;;) 
    {
        if (startRequests && unlocked) 
        {
            switch (flashState)
            {
                // ======================================================
                case FS_WRITE_SW_FP:
                // Step 1: Write App SW Fingerprint
                if (!swFingerprint)
                {
                    if (xSemaphoreTake(canMutex, portMAX_DELAY)) {
                        writeDataByIdAppSwFingerprint();
                        xSemaphoreGive(canMutex);
                    }
                }
                else {
                    flashState = FS_WRITE_APP_FP;
                }
                break;

                // ======================================================
                case FS_WRITE_APP_FP:
                // Step 2: Write App Data Fingerprint
                if (!appFingerprint)
                {
                    if (xSemaphoreTake(canMutex, portMAX_DELAY)) {
                        writeDataByIdAppDataFingerprint();
                        xSemaphoreGive(canMutex);
                    }
                }
                else {
                    flashState = FS_ERASE_MEMORY;
                }
                break;

                // ======================================================
                case FS_ERASE_MEMORY:
                // Step 3: Routine Control Erase Memory
                if (!erased)
                {
                    if (xSemaphoreTake(canMutex, portMAX_DELAY)) {
                        sendRoutineControleEraseMemory();
                        xSemaphoreGive(canMutex);
                    }
                }
                else {
                    flashState = FS_CHECK_ERASE;
                }
                break;

                // ======================================================
                case FS_CHECK_ERASE:
                // Step 4: Routine Control Check Routine
                if (!routineChecked)
                {
                    if (xSemaphoreTake(canMutex, portMAX_DELAY)) {
                        sendRoutineControlCheckRoutine();
                        xSemaphoreGive(canMutex);
                    }
                }
                else {
                    flashState = FS_DOWNLOAD_REQUEST;
                }
                break;

                // ======================================================
                case FS_DOWNLOAD_REQUEST:
                // Step 5: Send Download Request
                if (!downloadRequest)
                {
                    if (xSemaphoreTake(canMutex, portMAX_DELAY)) {
                        sendDownloadRequest();
                        xSemaphoreGive(canMutex);
                    }
                }
                else {
                    flashState = FS_TRANSFER_DATA;
                }
                break;

                // ======================================================
                case FS_TRANSFER_DATA:
                if (!allDataTransferred) {
                    flashState = FS_TRANSFER_SEND;
                } else {
                    flashState = FS_TRANSFER_EXIT;
                }
                break;

                case FS_TRANSFER_SEND:

                sendTransferBlock(firmwarePath);   // send ONE block only
                flashState = FS_TRANSFER_WAIT_ACK; // now wait for 0x76
                break;
                
                case FS_TRANSFER_WAIT_ACK:

                if (blockAckReceived) {

                    blockAckReceived = false;

                    // Check EOF NOW
                    if (!transferFile.available()) {
                        allDataTransferred = true;
                        transferFile.close();
                        flashInProgress = false;

                        flashState = FS_TRANSFER_EXIT;
                        break;
                    }

                    // Correct UDS block number increment (0..255)
                    blockCounter++;

                    flashState = FS_TRANSFER_DATA;
                }
                break;



                // ======================================================
                case FS_TRANSFER_EXIT:
                // Step 7: Request Transfer Exit
                if (!exited)
                {
                    if (xSemaphoreTake(canMutex, portMAX_DELAY)) {
                        sendRequestTransferExit();
                        xSemaphoreGive(canMutex);
                    }
                }
                else {
                    flashState = FS_CHECKSUM;
                }
                break;

                // ======================================================
                case FS_CHECKSUM:
                // Step 8: Checksum
                if (!checksumOk)
                {
                    if (xSemaphoreTake(canMutex, portMAX_DELAY)) {
                        sendRoutineControlChecksum();
                        xSemaphoreGive(canMutex);
                    }
                }
                else {
                    flashState = FS_CHECK_DEPS;
                }
                break;

                // ======================================================
                case FS_CHECK_DEPS:
                // Step 9: Check routine dependencies
                if (!finalDep)
                {
                    if (xSemaphoreTake(canMutex, portMAX_DELAY)) {
                        sendRoutineControlCheckRoutineDependencies();
                        xSemaphoreGive(canMutex);
                    }
                }
                else {
                    flashState = FS_ECU_RESET;
                }
                break;

                // ======================================================
                case FS_ECU_RESET:
                // Step 10: ECU Reset
                if (!reset)
                {
                    if (xSemaphoreTake(canMutex, portMAX_DELAY)) {
                        sendECUReset();
                        xSemaphoreGive(canMutex);
                    }
                }
                else {
                    flashState = FS_DONE;
                }
                break;

                // ======================================================
                case FS_DONE:
                    Serial.println("Firmware update process completed.");
                    logBuffer += "Firmware update process completed.\n";
                    vTaskSuspend(NULL);
                    break;
            }
        }

        vTaskDelay(50 / portTICK_PERIOD_MS); 
    }
}


// ================== Setup ==================
void setup() {
    Serial.begin(115200);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\nWiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    if (CAN.begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ) != CAN_OK) {
        Serial.println("CAN Initialization Failed.");
        while (1);
    }
    CAN.setMode(MCP_NORMAL);
    pinMode(CAN_INT_PIN, INPUT);

    if (!SPIFFS.begin()) { SPIFFS.format(); SPIFFS.begin(); }

    logQueue = xQueueCreate(128, sizeof(CANLogBin));
    if(logQueue == NULL) {
        Serial.println("Error creating log queue");
        while(1);
    } else {
        Serial.println("Log queue created successfully");
    }

    server.on("/start", HTTP_GET, []() { startRequests = true; startOTAUpdate = false; startTime = millis(); server.send(200, "text/plain", "UDS requests started"); Serial.println("UDS requests started"); });
    server.on("/stop", HTTP_GET, []() { startRequests = false; startOTAUpdate = false; server.send(200, "text/plain", "UDS requests stopped"); });
    // server.on("/log", HTTP_GET, []() { server.send(200, "text/plain", logBuffer); });
    // server.on("/clear_log", HTTP_GET, []() { logBuffer = ""; canLog.clear(); server.send(200, "text/plain", "Log cleared"); });
    server.on("/ota_update", HTTP_GET, []() { urlUpdate = server.arg("url_update"); startRequests = false; startOTAUpdate = true; server.send(200, "text/plain", "Starting OTA Update for version: " + urlUpdate); });
    server.on("/log", HTTP_GET, []() {
        CANLogBin entry;
        if (xQueueReceive(logQueue, &entry, 0) == pdTRUE) {
            String json = "{";
            json += "\"timestamp\":" + String(entry.timestamp, 6) + ",";
            json += "\"canId\":\"0x" + String(entry.canId, HEX) + "\",";
            json += "\"dlc\":" + String(entry.dlc) + ",";
            json += "\"data\":\"";
            
            for (int i = 0; i < entry.dlc; i++) {
                if (entry.data[i] < 0x10) json += "0";
                json += String(entry.data[i], HEX);
                if (i < entry.dlc - 1) json += " ";
            }
            
            json += "\",";
            json += "\"label\":" + String(entry.label);
            json += "}";
            server.send(200, "application/json", json);
        } else {
            server.send(204);
        }
    });
    server.begin();

    // --- Adicionar futuramente requests de ataques ---

    canMutex = xSemaphoreCreateMutex();

    // Start FreeRTOS tasks
    xTaskCreatePinnedToCore(canTask, "CAN Task", 8192, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(unlockTask, "Unlock Task", 8192, NULL, 2, NULL, 1);
    //xTaskCreatePinnedToCore(testerPresentTask, "Tester Present", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(flashTask, "Flash Task", 16384, NULL, 2, NULL, 1);
    


  }

// ================== Loop ==================
void loop() {
  // Handle HTTP server requests (start/stop, log, OTA)
  server.handleClient();

  // OTA update logic
  /*if (startOTAUpdate && !alreadyDownloaded) {
    if (downloadFirmware(urlUpdate.c_str(), "/firmware.bin")) {
      Serial.println("Firmware downloaded.");
      alreadyDownloaded = true;
    }
  }*/
  /*if(startRequests) {
    startOTAUpdate = false; server.send(200, "text/plain", "UDS requests started");
    Serial.println("UDS requests started");
  } */
  // Small delay to yield CPU to FreeRTOS tasks
  vTaskDelay(50 / portTICK_PERIOD_MS);
}

/*
void turnVehicleOn() { 
    byte udsRequestData[] = {0x05, 0x31, 0x01, 0x02, 0x00, 0xFF}; 
    sendMessage(FUNC_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Diagnostic Session Control"); 
} 
    
void turnLeftBeamLightOn() { 
    byte udsRequestData[] = {0x05, 0x2F, 0x50, 0x09, 0x03, 0xFF}; 
    sendMessage(FUNC_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Diagnostic Session Control"); 
} 

void turnRightBeamLightOn() { 
    byte udsRequestData[] = {0x05, 0x2F, 0x50, 0x0A, 0x03, 0xFF}; 
    sendMessage(FUNC_REQUEST_ID, udsRequestData, sizeof(udsRequestData), "Diagnostic Session Control"); 
 }
*/