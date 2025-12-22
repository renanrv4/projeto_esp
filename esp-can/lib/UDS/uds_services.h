#ifndef UDS_H
#define UDS_H

#define RAM_LOG_SIZE 32

enum LogLabel : uint8_t {
    LABEL_NORMAL = 0,
    LABEL_ATTACK = 1,
    LABEL_FAULT  = 2
};

struct CANLogBin {
    float timestamp;      // 4 bytes
    uint32_t canId;       // 4 bytes
    uint8_t dlc;          // 1 byte
    uint8_t data[8];      // 8 bytes
    uint8_t label;        // 1 byte
} __attribute__((packed));

extern CANLogBin ramLog[RAM_LOG_SIZE];
extern volatile uint8_t ramLogIndex;
extern volatile uint8_t ramLogCount;

void addToRamLog(const CANLogBin& entry);
void flushRamLogToSPIFFS();
String convertDecimalToHex(long int decimalNumber);
String convertBytesToString(const byte* data, size_t length);

void sendMessage(const long id, byte* data, byte len, const char* description, String labelClass="NORMAL");

void sendIdentificationCodeRequest(); //line 01
void sendTesterPresent(); //line 03
void sendReadDataByIdentifierVINCurrent(); //line 13
void sendReadDataByIdentifierVIN(); //line 18
void sendFlowControl(); // line 20
void sendExtendedDiagnosticSessionRequest();
void sendControlDTCSettingRequest(); //line 26
void sendCommunicationControlNoRxNoTxRequest(); //line 27
void sendProgrammingDiagnosticSessionRequest(); //line 28
void sendSecurityRequestSeed(); //line 31
void sendSecurityKey(String key); //line 33
String sendSeedToDllServer(String seed);
void sendKey(String key);
void writeDataByIdAppSwFingerprint(); //line 35
void writeDataByIdAppDataFingerprint(); //line 42
void sendRoutineControleEraseMemory(); //line 49
void sendTesterPresent();
void sendRoutineControlCheckRoutine(); //line 75
void sendDownloadRequest(); //line 77
//void sendStartTransferData(); //line 81
// Forward declarations
bool sendUDSChunkISO(File &file);
void sendStartTransferData(const char* filePath);
//bool waitUDSPositiveResponse(uint8_t expectedBlockCounter, unsigned long timeoutMs = 1000);

void sendRequestTransferExit(); //line 139652
void sendRoutineControlChecksum(); //line 139656
void sendRoutineControlCheckRoutineDependencies(); //line 139662
void sendECUReset(); //line 139666

//bool udsIsBlockAcked();

extern String logBuffer;
//extern int CS_PIN;
extern MCP_CAN CAN;
extern SemaphoreHandle_t canMutex;
extern SemaphoreHandle_t logMutex;
extern unsigned long startTime;

#endif