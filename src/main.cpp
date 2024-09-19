#include <Arduino.h>

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include "driver/i2s.h"

// WiFi credentials
const char* ssid = "tomap";
const char* password = "boogabooga";

// I2S configuration
#define I2S_DOUT      25
#define I2S_BCLK      27
#define I2S_LRC       26

// Web server
AsyncWebServer server(80);

// File upload name
const char* UPLOAD_FILE_NAME = "/uploaded.wav";

TaskHandle_t playbackTaskHandle = NULL;
SemaphoreHandle_t audioMutex = NULL;


// I2S configuration
i2s_config_t i2s_default_config = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
  .sample_rate = 22050,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
  .communication_format = I2S_COMM_FORMAT_STAND_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 8,
  .dma_buf_len = 64,
  .use_apll = false,
  .tx_desc_auto_clear = true,
  .fixed_mclk = 0
};

// I2S pin configuration
i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
};

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
  if(!index){
    Serial.printf("UploadStart: %s\n", filename.c_str());
    // Open the file on first call and store the handle in the request object
    request->_tempFile = LittleFS.open(UPLOAD_FILE_NAME, "w");
  }

  if(len){
    // Stream the incoming chunk to the opened file
    request->_tempFile.write(data, len);
    // print available heap
    Serial.printf("data_len: %u, free_heap: %u\n", len, ESP.getFreeHeap());
  }

  if(final){
    Serial.printf("UploadEnd: %s, %u B\n", filename.c_str(), index+len);
    // Close the file handle as the upload is now done
    request->_tempFile.close();
    request->redirect("/");
  }
}


struct WAVHeader {
    uint32_t sampleRate;
    uint16_t numChannels;
    uint16_t bitsPerSample;
};

bool readWAVHeader(File &file, WAVHeader &header) {
    char buffer[44];
    if (file.read((uint8_t*)buffer, 44) != 44) {
        return false;
    }

    // Check RIFF header
    if (strncmp(buffer, "RIFF", 4) != 0 || strncmp(buffer + 8, "WAVE", 4) != 0) {
        return false;
    }

    // Extract relevant information
    header.sampleRate = *((uint32_t*)(buffer + 24));
    header.numChannels = *((uint16_t*)(buffer + 22));
    header.bitsPerSample = *((uint16_t*)(buffer + 34));

    return true;
}

void configureI2S(const WAVHeader &header) {
    i2s_config_t i2s_config = i2s_default_config; //begin from default configuration
    i2s_config.sample_rate = header.sampleRate; //change sample rate according to WAV file
    i2s_config.bits_per_sample = (i2s_bits_per_sample_t)header.bitsPerSample; //change bits per sample according to WAV file
    
    i2s_driver_uninstall(I2S_NUM_0);
    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
}

void playWavFileTask(void *param) {
    const char* filename = (const char*)param;
    
    if (xSemaphoreTake(audioMutex, portMAX_DELAY) == pdTRUE) {
        File file = LittleFS.open(filename, "r");
        if (!file) {
            Serial.println("Failed to open file for reading");
            xSemaphoreGive(audioMutex);
            vTaskDelete(NULL);
            return;
        }

        WAVHeader wavHeader;
        if (!readWAVHeader(file, wavHeader)) {
            Serial.println("Invalid WAV file");
            file.close();
            xSemaphoreGive(audioMutex);
            vTaskDelete(NULL);
            return;
        }

        Serial.printf("WAV File: Sample Rate: %u, Channels: %u, Bits Per Sample: %u\n", 
                      wavHeader.sampleRate, wavHeader.numChannels, wavHeader.bitsPerSample);

        configureI2S(wavHeader);

        const size_t bufferSize = 1024;
        char buffer[bufferSize];
        size_t bytesRead;

        while ((bytesRead = file.read((uint8_t*)buffer, bufferSize)) > 0) {
            size_t bytesWritten = 0;
            while (bytesWritten < bytesRead) {
                size_t written = 0;
                esp_err_t result = i2s_write(I2S_NUM_0, (const char *)(buffer + bytesWritten), 
                                             bytesRead - bytesWritten, &written, portMAX_DELAY);
                if (result == ESP_OK) {
                    bytesWritten += written;
                } else {
                    Serial.printf("I2S write error: %d\n", result);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                taskYIELD();
            }
        }

        file.close();
        i2s_zero_dma_buffer(I2S_NUM_0);
        Serial.println("Playback finished");
        
        xSemaphoreGive(audioMutex);
    }
    
    playbackTaskHandle = NULL;
    vTaskDelete(NULL);
}

void handlePlayRequest(AsyncWebServerRequest *request) {
  if (playbackTaskHandle == NULL) {
    xTaskCreate(playWavFileTask, "playWavFileTask", 8192, (void*)UPLOAD_FILE_NAME, 1, &playbackTaskHandle);
    request->send(200, "text/plain", "Starting playback");
  } else {
    request->send(409, "text/plain", "Playback already in progress");
  }
}

void setup() {
  Serial.begin(115200);

  if (!LittleFS.begin(true)) {
    Serial.println("An error has occurred while mounting LittleFS");
    return;
  }

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connecting to WiFi...");
    delay(1000);    
  }
  Serial.println("Connected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  i2s_driver_install(I2S_NUM_0, &i2s_default_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);

  audioMutex = xSemaphoreCreateMutex();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS.open("/index.html", "r"), "/index.html", "text/html");
  });

  server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request){
    request->send(200);
  }, handleUpload);

  server.on("/play", HTTP_GET, handlePlayRequest);

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
    if (playbackTaskHandle == NULL) {
        request->send(200, "text/plain", "idle");
    } else {
        request->send(200, "text/plain", "playing");
    }
  });
  
  server.begin();
  Serial.println("Server started");
}

void loop() {
  // The main loop can be used for other tasks or left empty
  delay(100);
}