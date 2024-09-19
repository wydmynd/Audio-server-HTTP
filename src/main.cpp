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

// I2S configuration
i2s_config_t i2s_config = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
  .sample_rate = 11025,//22050,
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



void playWavFileTask(void *param) {
  const char* filename = (const char*)param;
  File file = LittleFS.open(filename, "r");
  unsigned int buff_counter = 0;
  if(!file){
    Serial.println("Failed to open file for reading");
    server.begin(); // Restart the server
    vTaskDelete(NULL); // Delete the task
    return;
  }

  // Skip WAV header
  file.seek(44);

  // Buffer for I2S data
  char buffer[1024];  // Declare buffer as char
  size_t bytesRead;

  while ((bytesRead = file.read((uint8_t*)buffer, sizeof(buffer))) > 0) { // Cast buffer to uint8_t*
    size_t bytesWritten = 0;
    while (bytesWritten < bytesRead) {
      size_t written = 0;
      esp_err_t result = i2s_write(I2S_NUM_0, (const char *)(buffer + bytesWritten), (bytesRead - bytesWritten), &written, pdMS_TO_TICKS(100)); // Use a timeout
      if (result == ESP_OK) {
        bytesWritten += written;
      } else {
        Serial.printf("I2S write error: %d\n", result);
        vTaskDelay(pdMS_TO_TICKS(10)); // Add a delay if there is an error
      }
      vTaskDelay(pdMS_TO_TICKS(1)); // Add a small delay to prevent the task watchdog from triggering
      yield(); // Allow the system to perform background tasks and reset the watchdog timer
      Serial.printf("cnt: %u, br: %u, bw: %u bwt: %u\n", buff_counter, bytesRead, written, bytesWritten); // print bytes read, bytes written, total bytes written
      buff_counter++;
    }
  }

  file.close();

  // Flush the remaining data in the I2S buffer
  i2s_zero_dma_buffer(I2S_NUM_0);

  Serial.println("Playback finished");
  
  server.begin(); // Restart the server
  delay(50); // Delay to allow the server to start
  vTaskDelete(NULL); // Delete the task
}

void playWavFile(const char* filename) {
  server.end(); // Stop the server to prevent new connections while playing audio
  delay(50); // Delay to allow the server to stop
  xTaskCreate(playWavFileTask, "playWavFileTask", 8192, (void*)filename, 1, NULL);
}


void setup() {
  Serial.begin(115200);

  // Initialize LittleFS
  if(!LittleFS.begin(true)){
    Serial.println("An error has occurred while mounting LittleFS");
    return;
  }

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Initialize I2S
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);

  // Set up web server routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });

  server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request){
    request->send(200);
  }, handleUpload);

  server.on("/play", HTTP_GET, [](AsyncWebServerRequest *request){
    playWavFile(UPLOAD_FILE_NAME);
    // request->send(200, "text/plain", "Playing audio");
  });

  // Start server
  server.begin();
}

void loop() {
  // Main loop is empty as we're using async web server
}
